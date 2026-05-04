// Reconstructs Spring archives back into FASTQ/FASTA output by decoding packed
// sequences and replaying aligned, unaligned, quality, and id streams.

#include "decompress.h"
#include "core_utils.h"
#include "dna_utils.h"
#include "fs_utils.h"
#include "integrity_utils.h"
#include "io_utils.h"
#include "params.h"
#include "parse_utils.h"
#include "progress.h"
#ifndef _WIN32
#include "raii.h"
#endif
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string_view>
#include <utility>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <algorithm>
#include <array>
#include <iterator>
#include <omp.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace spring {

namespace {

struct thread_range {
  uint64_t begin;
  uint64_t end;
};

struct reference_chunk {
  uint64_t start_offset;
  uint64_t size;
  std::string owned_data;
  const char *data = nullptr;
};

std::vector<char> read_binary_file(const std::string &path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input.is_open()) {
    throw std::runtime_error("Failed to open binary input: " + path);
  }
  const std::streamsize file_size = input.tellg();
  if (file_size < 0) {
    throw std::runtime_error("Failed to determine binary input size: " + path);
  }
  input.seekg(0, std::ios::beg);
  std::vector<char> bytes(static_cast<size_t>(file_size));
  if (file_size > 0 && !input.read(bytes.data(), file_size)) {
    throw std::runtime_error("Failed to read binary input: " + path);
  }
  return bytes;
}

std::vector<char>
archive_member_bytes(const decompression_archive_artifact &artifact,
                     const std::string &member_name) {
  const std::string &contents = artifact.require(member_name);
  return std::vector<char>(contents.begin(), contents.end());
}

std::vector<char>
decompress_archive_bsc_member(const decompression_archive_artifact &artifact,
                              const std::string &member_name,
                              const bool allow_raw_fallback = false) {
  const std::vector<char> compressed_bytes =
      archive_member_bytes(artifact, member_name);
  if (compressed_bytes.empty()) {
    return {};
  }
  try {
    return bsc_decompress_bytes(compressed_bytes);
  } catch (const std::exception &) {
    if (!allow_raw_fallback) {
      throw;
    }
    return compressed_bytes;
  }
}

class memory_cursor {
public:
  explicit memory_cursor(const std::vector<char> &bytes) : bytes_(bytes) {}

  template <typename T> T read(const std::string &label) {
    if (offset_ + sizeof(T) > bytes_.size()) {
      throw std::runtime_error("Corrupt archive: truncated " + label);
    }
    T value{};
    std::memcpy(&value, bytes_.data() + offset_, sizeof(T));
    offset_ += sizeof(T);
    return value;
  }

  char read_char(const std::string &label) { return read<char>(label); }

  void read_bytes(char *destination, size_t count, const std::string &label) {
    if (offset_ + count > bytes_.size()) {
      throw std::runtime_error("Corrupt archive: truncated " + label);
    }
    if (count > 0) {
      std::memcpy(destination, bytes_.data() + offset_, count);
      offset_ += count;
    }
  }

  [[nodiscard]] std::string read_line(const std::string &label) {
    if (offset_ > bytes_.size()) {
      throw std::runtime_error("Corrupt archive: truncated " + label);
    }
    const char *begin = bytes_.data() + offset_;
    const size_t remaining = bytes_.size() - offset_;
    const void *newline_ptr = std::memchr(begin, '\n', remaining);
    const size_t line_len =
        newline_ptr == nullptr
            ? remaining
            : static_cast<size_t>(static_cast<const char *>(newline_ptr) -
                                  begin);
    std::string line(begin, line_len);
    offset_ += line_len;
    if (newline_ptr != nullptr) {
      offset_ += 1;
    }
    return line;
  }

private:
  const std::vector<char> &bytes_;
  size_t offset_ = 0;
};

std::vector<std::string> slice_monolithic_id_blocks(
    const decompression_archive_artifact &artifact,
    const std::string &member_name, const uint64_t *file_len_thr,
    const uint32_t num_reads, const uint32_t num_reads_per_block) {
  if (!artifact.contains(member_name)) {
    return {};
  }

  const std::vector<char> packed_bytes =
      decompress_archive_bsc_member(artifact, member_name);
  const uint32_t num_blocks =
      (num_reads + num_reads_per_block - 1) / num_reads_per_block;
  if (num_blocks > compression_params::ReadMetadata::kFileLenThrSize) {
    throw std::runtime_error(
        std::string("Archive contains too many ID blocks (") +
        std::to_string(num_blocks) + ") for metadata array size (" +
        std::to_string(compression_params::ReadMetadata::kFileLenThrSize) +
        "). Increase array size in params.h.");
  }

  std::vector<std::string> blocks(static_cast<size_t>(num_blocks));
  size_t cursor = 0;
  for (uint32_t block_index = 0; block_index < num_blocks; ++block_index) {
    const uint64_t block_len = file_len_thr[block_index];
    if (block_len == 0) {
      continue;
    }
    if (cursor + block_len > packed_bytes.size()) {
      throw std::runtime_error(
          "Corrupt archive: truncated monolithic ID payload.");
    }
    blocks[block_index].assign(packed_bytes.data() + cursor,
                               static_cast<size_t>(block_len));
    cursor += static_cast<size_t>(block_len);
  }
  if (cursor != packed_bytes.size()) {
    throw std::runtime_error(
        "Corrupt archive: trailing bytes in monolithic ID payload.");
  }
  return blocks;
}

std::string make_decompress_step_log_message(const char *label,
                                             const uint32_t num_reads_done,
                                             const uint32_t num_reads_cur_step,
                                             const uint32_t num_blocks_done) {
  std::string message(label);
  message.append(std::to_string(num_reads_done));
  message.append(", reads_this_step=");
  message.append(std::to_string(num_reads_cur_step));
  message.append(", num_blocks_done=");
  message.append(std::to_string(num_blocks_done));
  return message;
}

class reference_sequence_store {
public:
  reference_sequence_store(const reference_sequence_store &) = delete;
  reference_sequence_store &
  operator=(const reference_sequence_store &) = delete;
  reference_sequence_store(reference_sequence_store &&) = delete;
  reference_sequence_store &operator=(reference_sequence_store &&) = delete;

  reference_sequence_store(const decompression_archive_artifact &artifact,
                           const std::string &packed_seq_path,
                           const int encoding_thread_count,
                           const int decode_thread_count,
                           const compression_params &cp) {
    std::vector<std::string> decoded_chunks = decompress_unpack_seq_chunks(
        artifact, packed_seq_path, encoding_thread_count, decode_thread_count,
        cp);

    chunks_.reserve(static_cast<size_t>(encoding_thread_count));
    uint64_t next_start_offset = 0;
    for (int encoding_thread_id = 0; encoding_thread_id < encoding_thread_count;
         encoding_thread_id++) {
      reference_chunk chunk;
      chunk.size = cp.read_info.file_len_seq_thr[encoding_thread_id];
      chunk.start_offset = next_start_offset;
      next_start_offset += chunk.size;
      if (static_cast<size_t>(encoding_thread_id) < decoded_chunks.size()) {
        chunk.owned_data = std::move(decoded_chunks[encoding_thread_id]);
      }
      start_offsets_.push_back(chunk.start_offset);
      chunks_.push_back(std::move(chunk));
      chunks_.back().data = chunks_.back().owned_data.data();
      total_size_ = next_start_offset;
    }
  }

  ~reference_sequence_store() = default;

  [[nodiscard]] std::string read(uint64_t start_offset,
                                 uint32_t read_length) const {
    std::string read;
    read.reserve(read_length);

    uint64_t remaining = read_length;
    uint64_t current_offset = start_offset;
    size_t chunk_index = find_chunk_index(current_offset);
    while (remaining > 0) {
      const reference_chunk &chunk = chunks_[chunk_index];
      const uint64_t offset_in_chunk = current_offset - chunk.start_offset;
      const uint64_t copy_size =
          std::min<uint64_t>(remaining, chunk.size - offset_in_chunk);
      read.append(chunk.data + offset_in_chunk, static_cast<size_t>(copy_size));

      current_offset += copy_size;
      remaining -= copy_size;
      chunk_index++;
    }

    return read;
  }

private:
  [[nodiscard]] size_t find_chunk_index(const uint64_t offset) const {
    auto it = std::ranges::upper_bound(start_offsets_, offset);
    if (it == start_offsets_.begin()) {
      throw std::runtime_error(
          "Reference offset out of range (offset=" + std::to_string(offset) +
          ", total=" + std::to_string(total_size_) + ")");
    }
    size_t chunk_index =
        static_cast<size_t>(std::prev(it) - start_offsets_.begin());
    const auto &chunk = chunks_[chunk_index];
    if (offset >= chunk.start_offset + chunk.size) {
      throw std::runtime_error(
          "Reference offset out of range (offset=" + std::to_string(offset) +
          ", chunk_idx=" + std::to_string(chunk_index) +
          ", chunk_start=" + std::to_string(chunk.start_offset) +
          ", chunk_size=" + std::to_string(chunk.size) +
          ", total=" + std::to_string(total_size_) +
          ", num_chunks=" + std::to_string(chunks_.size()) + ")");
    }
    return chunk_index;
  }

  std::vector<uint64_t> start_offsets_;
  std::vector<reference_chunk> chunks_;
  uint64_t total_size_ = 0;
};

thread_range split_thread_range(const uint64_t item_count, const int thread_id,
                                const int thread_count) {
  thread_range range;
  range.begin = uint64_t(thread_id) * item_count / thread_count;
  range.end = uint64_t(thread_id + 1) * item_count / thread_count;
  if (thread_id == thread_count - 1)
    range.end = item_count;
  return range;
}

std::string block_file_path(const std::string &base_path,
                            const uint32_t block_num) {
  std::string path = base_path;
  path.push_back('.');
  path.append(std::to_string(block_num));
  return path;
}

std::string compressed_block_file_path(const std::string &base_path,
                                       const uint32_t block_num) {
  std::string path = block_file_path(base_path, block_num);
  path.append(".bsc");
  return path;
}

bool is_unaligned_block_path(const std::string &path) {
  return path.find("read_unaligned.txt.") != std::string::npos;
}

void copy_binary_file(const std::string &input_path,
                      const std::string &output_path) {
  std::ifstream input(input_path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("Failed to open input file for copy: " +
                             input_path);
  }
  std::ofstream output(output_path, std::ios::binary);
  if (!output.is_open()) {
    throw std::runtime_error("Failed to open output file for copy: " +
                             output_path);
  }
  output << input.rdbuf();
}

void read_raw_string_block(const std::string &input_path,
                           std::string *string_array,
                           const uint32_t string_count,
                           const uint32_t *string_lengths) {
  std::ifstream input(input_path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("Failed to open raw string block: " + input_path);
  }
  for (uint32_t i = 0; i < string_count; i++) {
    string_array[i].resize(string_lengths[i]);
    input.read(string_array[i].data(),
               static_cast<std::streamsize>(string_lengths[i]));
    if (!input) {
      throw std::runtime_error("Failed to read raw string block: " +
                               input_path);
    }
  }
}

void decompress_bsc_block(const std::string &base_path,
                          const uint32_t block_num) {
  const std::string output_path = block_file_path(base_path, block_num);
  const std::string input_path =
      compressed_block_file_path(base_path, block_num);
  if (is_unaligned_block_path(input_path)) {
    // Backward compatibility: older archives may store unaligned blocks as raw
    // bytes with a .bsc suffix, while newer archives store proper BSC blocks.
    try {
      safe_bsc_decompress(input_path, output_path);
    } catch (const std::exception &) {
      copy_binary_file(input_path, output_path);
    }
  } else {
    safe_bsc_decompress(input_path, output_path);
  }
  safe_remove_file(input_path);
}

void decompress_read_length_block(const std::string &base_path,
                                  const uint32_t block_num,
                                  uint32_t *read_lengths_buffer,
                                  const uint64_t buffer_offset,
                                  const uint32_t read_count) {
  const std::string compressed_path =
      compressed_block_file_path(base_path, block_num);
  const std::string output_path = block_file_path(base_path, block_num);
  try {
    safe_bsc_decompress(compressed_path, output_path);
  } catch (const std::exception &) {
    if (compressed_path.find("readlength_") != std::string::npos) {
      // Backward compatibility: older archives used a .bsc suffix for raw
      // read-length blocks.
      copy_binary_file(compressed_path, output_path);
    } else {
      throw;
    }
  }
  safe_remove_file(compressed_path);

  std::ifstream read_length_input(output_path, std::ios::binary);
  for (uint32_t read_index = 0; read_index < read_count; read_index++) {
    read_length_input.read(
        byte_ptr(&read_lengths_buffer[buffer_offset + read_index]),
        sizeof(uint32_t));
  }
  read_length_input.close();
  safe_remove_file(output_path);
}

uint32_t compute_thread_read_count(const uint32_t step_read_count,
                                   uint32_t num_reads_per_block,
                                   uint64_t thread_id) {
  return std::min((uint64_t)step_read_count,
                  (thread_id + 1) * (uint64_t)num_reads_per_block) -
         thread_id * (uint64_t)num_reads_per_block;
}

} // namespace

namespace {
// Restore a stripped poly-A/T tail onto a read (and optionally its quality).
void append_tail(std::string &read_str, std::string *quality_str,
                 uint16_t tail_info, const std::string *tail_qual) {
  if (tail_info == 0)
    return;
  const char base = (tail_info & 1) ? 'T' : 'A';
  const uint32_t run = tail_info >> 1;
  read_str.append(run, base);
  if (quality_str) {
    if (tail_qual) {
      quality_str->append(*tail_qual);
    } else {
      quality_str->append(run, 'I');
    }
  }
}

void append_atac_adapter_tail(std::string &read_str, std::string *quality_str,
                              uint8_t adapter_info,
                              const std::string *tail_qual) {
  static constexpr std::array<std::string_view, 2> kAdapters = {
      "CTGTCTCTTATACACATCT", "AGATGTGTATAAGAGACAG"};
  if (adapter_info == 0)
    return;

  const uint8_t adapter_id = adapter_info & 1;
  const uint32_t overlap = adapter_info >> 1;
  const std::string_view adapter = kAdapters[adapter_id];
  if (overlap > adapter.size()) {
    throw std::runtime_error("Corrupt ATAC adapter metadata: overlap exceeds "
                             "adapter length");
  }

  read_str.append(adapter.substr(0, overlap));
  if (quality_str) {
    if (tail_qual) {
      quality_str->append(*tail_qual);
    } else {
      quality_str->append(overlap, 'I');
    }
  }
}

void append_grouped_index_suffix_to_id(std::string &id,
                                       const std::string &index_read_1,
                                       const std::string *index_read_2) {
  if (id.empty() || id.back() != ':') {
    throw std::runtime_error("Corrupt grouped sc-RNA index metadata: stripped "
                             "ID prefix missing ':'");
  }

  id.append(index_read_1);
  if (index_read_2) {
    id.push_back('+');
    id.append(*index_read_2);
  }
}
} // namespace

void write_fastq_block(std::ostream &output_stream, std::string *id_buffer,
                       std::string *read_buffer,
                       const std::string *quality_array,
                       uint32_t output_read_count, int num_thr,
                       bool gzip_output, bool bgzf_output,
                       int compression_level, bool use_crlf, bool fasta_mode,
                       bool quality_header_has_id);

FileDecompressionSink::FileDecompressionSink(const std::string &outfile_1,
                                             const std::string &outfile_2,
                                             const compression_params &cp,
                                             const int (&compression_levels)[2],
                                             const bool (&gzip)[2],
                                             const bool (&bgzf)[2],
                                             const bool (&write_enabled)[2])
    : fasta_mode(cp.encoding.fasta_mode), num_thr(cp.encoding.num_thr),
      paired_end(cp.encoding.paired_end) {
  should_gzip[0] = gzip[0];
  should_gzip[1] = gzip[1];
  should_bgzf[0] = bgzf[0];
  should_bgzf[1] = bgzf[1];
  write_enabled_[0] = write_enabled[0];
  write_enabled_[1] = write_enabled[1];
  compression_level_[0] = compression_levels[0];
  compression_level_[1] = compression_levels[1];
  use_crlf_[0] = cp.encoding.use_crlf_by_stream[0];
  use_crlf_[1] = cp.encoding.use_crlf_by_stream[1];
  quality_header_has_id_[0] = cp.read_info.quality_header_has_id_by_stream[0];
  quality_header_has_id_[1] = cp.read_info.quality_header_has_id_by_stream[1];

  if (write_enabled_[0]) {
    output_streams[0].open(outfile_1, std::ios::binary);
    if (!output_streams[0])
      throw std::runtime_error("Failed to open output file: " + outfile_1);
  }
  if (paired_end && write_enabled_[1]) {
    output_streams[1].open(outfile_2, std::ios::binary);
    if (!output_streams[1])
      throw std::runtime_error("Failed to open output file: " + outfile_2);
  }
}

FileDecompressionSink::~FileDecompressionSink() = default;

void FileDecompressionSink::consume_step(std::string *id_buffer,
                                         std::string *read_buffer,
                                         const std::string *quality_buffer,
                                         uint32_t count, int stream_index) {
  for (uint32_t i = 0; i < count; ++i) {
    update_record_crc(sequence_crc_[stream_index], read_buffer[i]);
    if (quality_buffer) {
      update_record_crc(quality_crc_[stream_index], quality_buffer[i]);
    }
    update_record_crc(id_crc_[stream_index], id_buffer[i]);
  }
  if (!write_enabled_[stream_index]) {
    return;
  }
  write_fastq_block(output_streams[stream_index], id_buffer, read_buffer,
                    quality_buffer, count, num_thr, should_gzip[stream_index],
                    should_bgzf[stream_index], compression_level_[stream_index],
                    use_crlf_[stream_index], fasta_mode,
                    quality_header_has_id_[stream_index]);
}

void write_step_output(std::ofstream &output_stream, std::string *id_buffer,
                       std::string *read_buffer,
                       const std::string *quality_array,
                       const uint32_t output_read_count, const int num_thr,
                       const bool gzip_output, const bool bgzf_output,
                       const int compression_level, const bool use_crlf,
                       const bool fasta_mode,
                       const bool quality_header_has_id) {
  write_fastq_block(output_stream, id_buffer, read_buffer, quality_array,
                    output_read_count, num_thr, gzip_output, bgzf_output,
                    compression_level, use_crlf, fasta_mode,
                    quality_header_has_id);
}

std::string decode_packed_sequence_chunk_bytes(
    const std::vector<char> &packed_bytes, const int encoding_thread_id,
    const uint64_t num_bases, bool bisulfite_ternary) {
  SPRING_LOG_DEBUG("decode_packed_sequence_chunk start: chunk=" +
                   std::to_string(encoding_thread_id) +
                   ", num_bases=" + std::to_string(num_bases) +
                   ", ternary=" + (bisulfite_ternary ? "yes" : "no"));
  static const char base_lookup[4] = {'A', 'G', 'C', 'T'};

  std::string decoded;
  decoded.reserve(static_cast<size_t>(num_bases));
  uint64_t bases_decoded = 0;
  size_t packed_offset = 0;

  if (!bisulfite_ternary) {
    while (packed_offset < packed_bytes.size() && bases_decoded < num_bases) {
      uint8_t byte = static_cast<uint8_t>(packed_bytes[packed_offset++]);
      for (int i = 0; i < 4 && bases_decoded < num_bases; i++) {
        decoded.push_back(base_lookup[byte & 3]);
        byte >>= 2;
        bases_decoded++;
      }
    }
  } else {
    while (packed_offset < packed_bytes.size() && bases_decoded < num_bases) {
      uint8_t byte = static_cast<uint8_t>(packed_bytes[packed_offset++]);

      if (byte < 243) {
        // Standard ternary: 5 bases per byte
        for (int k = 0; k < 5 && bases_decoded < num_bases; k++) {
          uint8_t val = byte % 3;
          byte /= 3;
          static constexpr char kBases[3] = {'A', 'G', 'T'};
          decoded.push_back(kBases[val]);
          bases_decoded++;
        }
      } else {
        // Escape: 2 bits per base, 5 bases in uint16_t
        if (packed_offset + sizeof(uint16_t) > packed_bytes.size()) {
          throw std::runtime_error(
              "Corrupt archive: truncated bisulfite sequence escape block.");
        }
        uint16_t escape = 0;
        std::memcpy(&escape, packed_bytes.data() + packed_offset,
                    sizeof(uint16_t));
        packed_offset += sizeof(uint16_t);
        for (int k = 0; k < 5 && bases_decoded < num_bases; k++) {
          uint8_t val = (escape >> (2 * k)) & 3;
          decoded.push_back(base_lookup[val]);
          bases_decoded++;
        }
      }
    }
  }
  SPRING_LOG_DEBUG("decode_packed_sequence_chunk done: chunk=" +
                   std::to_string(encoding_thread_id) +
                   ", bases=" + std::to_string(bases_decoded));
  if (bases_decoded != num_bases) {
    throw std::runtime_error(
        "Corrupt archive: sequence chunk decoded base count mismatch.");
  }
  return decoded;
}

bool is_gzip_output_path(const std::string &output_path) {
  return has_suffix(output_path, ".gz");
}

void open_output_files(std::ofstream (&output_streams)[2],
                       const std::string (&output_paths)[2],
                       const bool paired_end,
                       const bool (& /*gzip_outputs*/)[2]) {
  for (int stream_index = 0; stream_index < 2; stream_index++) {
    if (stream_index == 1 && !paired_end)
      continue;
    output_streams[stream_index].open(output_paths[stream_index],
                                      std::ios::binary);
  }
}

void validate_output_files(std::ofstream (&output_streams)[2],
                           const bool paired_end) {
  if (!output_streams[0].is_open())
    throw std::runtime_error("Error opening output file");
  if (paired_end && !output_streams[1].is_open())
    throw std::runtime_error("Error opening output file");
}

uint64_t compute_num_reads_per_step(const uint32_t num_reads,
                                    const uint32_t num_reads_per_block,
                                    const int num_thr, const bool paired_end) {
  uint64_t num_reads_per_step =
      static_cast<uint64_t>(num_thr) * num_reads_per_block;
  const uint64_t total_reads = paired_end ? num_reads / 2 : num_reads;
  if (num_reads_per_step > total_reads)
    num_reads_per_step = total_reads;
  return num_reads_per_step;
}

uint32_t compute_num_reads_cur_step(const uint32_t num_reads,
                                    const uint32_t num_reads_done,
                                    const uint64_t num_reads_per_step,
                                    const bool paired_end) {
  const uint32_t total_reads = paired_end ? num_reads / 2 : num_reads;
  if (num_reads_done + num_reads_per_step >= total_reads)
    return total_reads - num_reads_done;
  return static_cast<uint32_t>(num_reads_per_step);
}

int resolve_archive_encoding_thread_count(const compression_params &cp) {
  const int declared = cp.encoding.num_thr;
  if (declared <= 0 ||
      std::cmp_greater(declared,
                       compression_params::ReadMetadata::kFileLenThrSize)) {
    throw std::runtime_error(
        "Invalid encoding thread count in archive metadata: " +
        std::to_string(declared));
  }

  int inferred = 0;
  for (size_t i = 0; i < compression_params::ReadMetadata::kFileLenThrSize;
       i++) {
    if (cp.read_info.file_len_seq_thr[i] != 0 ||
        cp.read_info.file_len_id_thr[i] != 0) {
      inferred = static_cast<int>(i) + 1;
    }
  }

  const int resolved = std::max(declared, inferred);
  if (resolved != declared) {
    SPRING_LOG_DEBUG("Archive metadata thread count mismatch: declared=" +
                     std::to_string(declared) +
                     ", inferred_from_lengths=" + std::to_string(inferred) +
                     ", using=" + std::to_string(resolved));
  }
  return resolved;
}

void set_dec_noise_array(std::array<std::array<char, 128>, 128> &dec_noise);

void decompress_short(const decompression_archive_artifact &artifact,
                      DecompressionSink &sink, compression_params &cp,
                      int decoding_num_thr) {
  (void)decoding_num_thr;
  SPRING_LOG_DEBUG(
      "decompress_short start: scratch_dir=" + artifact.scratch_dir +
      ", num_reads=" + std::to_string(cp.read_info.num_reads) +
      ", paired_end=" + std::string(cp.encoding.paired_end ? "true" : "false") +
      ", preserve_order=" +
      std::string(cp.encoding.preserve_order ? "true" : "false") +
      ", preserve_id=" +
      std::string(cp.encoding.preserve_id ? "true" : "false") +
      ", preserve_quality=" +
      std::string(cp.encoding.preserve_quality ? "true" : "false"));

  const std::string file_seq = "read_seq.bin";
  const std::string file_flag = "read_flag.txt";
  const std::string file_pos = "read_pos.bin";
  const std::string file_pos_pair = "read_pos_pair.bin";
  const std::string file_rc = "read_rev.txt";
  const std::string file_rc_pair = "read_rev_pair.txt";
  const std::string file_readlength = "read_lengths.bin";
  const std::string file_unaligned = "read_unaligned.txt";
  const std::string file_noise = "read_noise.txt";
  const std::string file_noisepos = "read_noisepos.bin";
  const std::array<std::string, 2> input_quality_paths = {"quality_1",
                                                          "quality_2"};
  const std::array<std::string, 2> input_id_paths = {"id_1", "id_2"};

  const uint32_t num_reads = cp.read_info.num_reads;
  const uint8_t paired_id_code = cp.read_info.paired_id_code;
  const bool paired_id_match = cp.read_info.paired_id_match;
  const uint32_t num_reads_per_block = cp.encoding.num_reads_per_block;
  const bool paired_end = cp.encoding.paired_end;
  const bool preserve_id = cp.encoding.preserve_id;
  const bool preserve_quality = cp.encoding.preserve_quality;
  const bool preserve_order = cp.encoding.preserve_order;
  const bool poly_at_stripped = cp.encoding.poly_at_stripped;
  const bool atac_adapter_stripped = cp.encoding.atac_adapter_stripped;
  const bool cb_prefix_stripped = cp.encoding.cb_prefix_stripped;
  const bool index_id_suffix_reconstructed =
      cp.encoding.index_id_suffix_reconstructed;
  const uint32_t cb_prefix_len = cp.encoding.cb_prefix_len;
  const int archive_encoding_thread_count =
      resolve_archive_encoding_thread_count(cp);

  std::array<std::vector<std::string>, 2> monolithic_id_blocks;
  bool monolithic_id[2] = {false, false};
  if (preserve_id) {
    const uint32_t file_read_count = paired_end ? (num_reads / 2) : num_reads;
    monolithic_id_blocks[0] = slice_monolithic_id_blocks(
        artifact, input_id_paths[0] + ".bsc", cp.read_info.file_len_id_thr,
        file_read_count, num_reads_per_block);
    monolithic_id[0] = !monolithic_id_blocks[0].empty();
    if (paired_end && !paired_id_match) {
      monolithic_id_blocks[1] = slice_monolithic_id_blocks(
          artifact, input_id_paths[1] + ".bsc", cp.read_info.file_len_id_thr,
          file_read_count, num_reads_per_block);
      monolithic_id[1] = !monolithic_id_blocks[1].empty();
    }
  }

  std::vector<char> tail_bytes_1;
  std::vector<char> tail_bytes_2;
  memory_cursor *tail_cursor_1 = nullptr;
  memory_cursor *tail_cursor_2 = nullptr;
  std::optional<memory_cursor> tail_cursor_storage_1;
  std::optional<memory_cursor> tail_cursor_storage_2;
  if (poly_at_stripped) {
    tail_bytes_1 = archive_member_bytes(artifact, "tail_1.bin");
    tail_cursor_storage_1.emplace(tail_bytes_1);
    tail_cursor_1 = &*tail_cursor_storage_1;
    if (paired_end) {
      tail_bytes_2 = archive_member_bytes(artifact, "tail_2.bin");
      tail_cursor_storage_2.emplace(tail_bytes_2);
      tail_cursor_2 = &*tail_cursor_storage_2;
    }
  }

  std::vector<char> adapter_bytes_1;
  std::vector<char> adapter_bytes_2;
  std::optional<memory_cursor> adapter_cursor_1;
  std::optional<memory_cursor> adapter_cursor_2;
  if (atac_adapter_stripped) {
    adapter_bytes_1 =
        decompress_archive_bsc_member(artifact, "atac_adapter_1.bin.bsc");
    adapter_cursor_1.emplace(adapter_bytes_1);
    if (paired_end) {
      adapter_bytes_2 =
          decompress_archive_bsc_member(artifact, "atac_adapter_2.bin.bsc");
      adapter_cursor_2.emplace(adapter_bytes_2);
    }
  }

  std::vector<char> cb_seq_bytes;
  std::vector<char> cb_qual_bytes;
  size_t cb_seq_cursor = 0;
  size_t cb_qual_cursor = 0;
  if (cb_prefix_stripped) {
    cb_seq_bytes = decompress_archive_bsc_member(artifact, "cb_prefix.dna.bsc");
    if (preserve_quality) {
      cb_qual_bytes =
          decompress_archive_bsc_member(artifact, "cb_prefix.qual.bsc");
    }
  }

  const uint64_t num_reads_per_step =
      compute_num_reads_per_step(num_reads, num_reads_per_block,
                                 archive_encoding_thread_count, paired_end);

  std::vector<std::string> read_buffer_1(
      static_cast<size_t>(num_reads_per_step));
  std::vector<std::string> read_buffer_2;
  if (paired_end) {
    read_buffer_2.resize(static_cast<size_t>(num_reads_per_step));
  }
  std::vector<std::string> id_buffer(static_cast<size_t>(num_reads_per_step));
  std::vector<std::string> quality_buffer;
  if (preserve_quality) {
    quality_buffer.resize(static_cast<size_t>(num_reads_per_step));
  }
  std::vector<uint32_t> read_lengths_buffer_1(
      static_cast<size_t>(num_reads_per_step));
  std::vector<uint32_t> read_lengths_buffer_2;
  if (paired_end) {
    read_lengths_buffer_2.resize(static_cast<size_t>(num_reads_per_step));
  }
  std::array<std::array<char, 128>, 128> decoded_noise_table;
  set_dec_noise_array(decoded_noise_table);

  omp_set_num_threads(archive_encoding_thread_count);
  reference_sequence_store seq(artifact, file_seq,
                               archive_encoding_thread_count,
                               archive_encoding_thread_count, cp);

  uint32_t num_blocks_done = 0;
  uint32_t num_reads_done = 0;
  for (;;) {
    const uint32_t num_reads_cur_step = compute_num_reads_cur_step(
        num_reads, num_reads_done, num_reads_per_step, paired_end);
    if (num_reads_cur_step == 0) {
      break;
    }
    SPRING_LOG_DEBUG(make_decompress_step_log_message(
        "decompress_short step: num_reads_done=", num_reads_done,
        num_reads_cur_step, num_blocks_done));

    for (int stream_index = 0; stream_index < 2; ++stream_index) {
      if (stream_index == 1 && !paired_end) {
        continue;
      }

      std::exception_ptr omp_exception;
#pragma omp parallel
      {
        try {
          const uint64_t thread_id = omp_get_thread_num();
          if (thread_id * num_reads_per_block < num_reads_cur_step) {
            const uint32_t thread_read_count = compute_thread_read_count(
                num_reads_cur_step, num_reads_per_block, thread_id);
            const uint64_t buffer_offset = thread_id * num_reads_per_block;
            const uint32_t block_num =
                num_blocks_done + static_cast<uint32_t>(thread_id);

            if (stream_index == 0) {
              const std::vector<char> flag_bytes =
                  decompress_archive_bsc_member(
                      artifact,
                      compressed_block_file_path(file_flag, block_num));
              const std::vector<char> noise_bytes =
                  decompress_archive_bsc_member(
                      artifact,
                      compressed_block_file_path(file_noise, block_num));
              const std::vector<char> noisepos_bytes =
                  decompress_archive_bsc_member(
                      artifact,
                      compressed_block_file_path(file_noisepos, block_num));
              const std::vector<char> pos_bytes = decompress_archive_bsc_member(
                  artifact, compressed_block_file_path(file_pos, block_num));
              const std::vector<char> rc_bytes = decompress_archive_bsc_member(
                  artifact, compressed_block_file_path(file_rc, block_num));
              const std::vector<char> unaligned_bytes =
                  decompress_archive_bsc_member(
                      artifact,
                      compressed_block_file_path(file_unaligned, block_num),
                      true);
              const std::vector<char> readlength_bytes =
                  decompress_archive_bsc_member(
                      artifact,
                      compressed_block_file_path(file_readlength, block_num));
              memory_cursor flag_cursor(flag_bytes);
              memory_cursor noise_cursor(noise_bytes);
              memory_cursor noisepos_cursor(noisepos_bytes);
              memory_cursor pos_cursor(pos_bytes);
              memory_cursor rc_cursor(rc_bytes);
              memory_cursor unaligned_cursor(unaligned_bytes);
              memory_cursor readlength_cursor(readlength_bytes);
              std::optional<memory_cursor> pos_pair_cursor;
              std::optional<memory_cursor> rc_pair_cursor;
              std::optional<std::vector<char>> pos_pair_bytes;
              std::optional<std::vector<char>> rc_pair_bytes;
              if (paired_end) {
                pos_pair_bytes.emplace(decompress_archive_bsc_member(
                    artifact,
                    compressed_block_file_path(file_pos_pair, block_num)));
                rc_pair_bytes.emplace(decompress_archive_bsc_member(
                    artifact,
                    compressed_block_file_path(file_rc_pair, block_num)));
                pos_pair_cursor.emplace(*pos_pair_bytes);
                rc_pair_cursor.emplace(*rc_pair_bytes);
              }

              uint64_t previous_position = 0;
              bool first_read_of_block = true;
              for (uint32_t i = static_cast<uint32_t>(buffer_offset);
                   i < buffer_offset + thread_read_count; ++i) {
                const char read_flag = flag_cursor.read_char("read flag");
                const uint16_t read_length =
                    readlength_cursor.read<uint16_t>("read length");
                read_lengths_buffer_1[i] = read_length;
                const bool read_1_is_singleton =
                    (read_flag == '2') || (read_flag == '4');

                uint64_t read_1_position = 0;
                char read_1_orientation = 'd';
                if (!read_1_is_singleton) {
                  if (preserve_order) {
                    read_1_position =
                        pos_cursor.read<uint64_t>("read position");
                  } else if (first_read_of_block) {
                    first_read_of_block = false;
                    read_1_position =
                        pos_cursor.read<uint64_t>("first block position");
                    previous_position = read_1_position;
                  } else {
                    const uint16_t position_delta_16 =
                        pos_cursor.read<uint16_t>("position delta");
                    if (position_delta_16 == 65535) {
                      read_1_position =
                          pos_cursor.read<uint64_t>("absolute position");
                    } else {
                      read_1_position = previous_position + position_delta_16;
                    }
                    previous_position = read_1_position;
                  }

                  read_1_orientation = rc_cursor.read_char("read orientation");
                  std::string read =
                      seq.read(read_1_position, read_lengths_buffer_1[i]);
                  const std::string noise_codes =
                      noise_cursor.read_line("noise code stream");
                  uint16_t previous_noise_position = 0;
                  for (char noise_code : noise_codes) {
                    uint16_t noise_position_delta =
                        noisepos_cursor.read<uint16_t>("noise position delta");
                    noise_position_delta += previous_noise_position;
                    read[noise_position_delta] =
                        decoded_noise_table[static_cast<uint8_t>(
                            read[noise_position_delta])]
                                           [static_cast<uint8_t>(noise_code)];
                    previous_noise_position = noise_position_delta;
                  }
                  read_buffer_1[i] =
                      (read_1_orientation == 'd')
                          ? read
                          : reverse_complement(read, read_lengths_buffer_1[i]);
                } else {
                  read_buffer_1[i].resize(read_lengths_buffer_1[i]);
                  unaligned_cursor.read_bytes(read_buffer_1[i].data(),
                                              read_lengths_buffer_1[i],
                                              "unaligned read");
                }

                if (paired_end) {
                  const bool read_2_is_singleton =
                      (read_flag == '2') || (read_flag == '3');
                  read_lengths_buffer_2[i] =
                      readlength_cursor.read<uint16_t>("mate read length");
                  if (!read_2_is_singleton) {
                    uint64_t read_2_position = 0;
                    char read_2_orientation = 'd';
                    if (read_flag == '1' || read_flag == '4') {
                      read_2_position =
                          pos_cursor.read<uint64_t>("mate position");
                      read_2_orientation =
                          rc_cursor.read_char("mate orientation");
                    } else {
                      const int16_t mate_position_delta =
                          pos_pair_cursor->read<int16_t>("mate position delta");
                      const int64_t mate_position_signed =
                          static_cast<int64_t>(read_1_position) +
                          static_cast<int64_t>(mate_position_delta);
                      if (mate_position_signed < 0) {
                        throw std::runtime_error(
                            "Corrupt archive: negative mate position");
                      }
                      read_2_position =
                          static_cast<uint64_t>(mate_position_signed);
                      const char relative_orientation_flag =
                          rc_pair_cursor->read_char(
                              "relative mate orientation");
                      read_2_orientation =
                          (relative_orientation_flag == '0')
                              ? ((read_1_orientation == 'd') ? 'r' : 'd')
                              : ((read_1_orientation == 'd') ? 'd' : 'r');
                    }

                    std::string read =
                        seq.read(read_2_position, read_lengths_buffer_2[i]);
                    const std::string noise_codes =
                        noise_cursor.read_line("mate noise code stream");
                    uint16_t previous_noise_position = 0;
                    for (char noise_code : noise_codes) {
                      uint16_t noise_position_delta =
                          noisepos_cursor.read<uint16_t>(
                              "mate noise position delta");
                      noise_position_delta += previous_noise_position;
                      read[noise_position_delta] =
                          decoded_noise_table[static_cast<uint8_t>(
                              read[noise_position_delta])]
                                             [static_cast<uint8_t>(noise_code)];
                      previous_noise_position = noise_position_delta;
                    }
                    read_buffer_2[i] =
                        (read_2_orientation == 'd')
                            ? read
                            : reverse_complement(read,
                                                 read_lengths_buffer_2[i]);
                  } else {
                    read_buffer_2[i].resize(read_lengths_buffer_2[i]);
                    unaligned_cursor.read_bytes(read_buffer_2[i].data(),
                                                read_lengths_buffer_2[i],
                                                "unaligned mate read");
                  }
                }
              }
            }

            if (preserve_quality) {
              const std::string quality_member =
                  input_quality_paths[stream_index] + "." +
                  std::to_string(block_num);
              if (stream_index == 0) {
                safe_bsc_str_array_decompress_bytes(
                    artifact.require(quality_member), quality_member,
                    quality_buffer.data() + buffer_offset, thread_read_count,
                    read_lengths_buffer_1.data() + buffer_offset);
              } else {
                safe_bsc_str_array_decompress_bytes(
                    artifact.require(quality_member), quality_member,
                    quality_buffer.data() + buffer_offset, thread_read_count,
                    read_lengths_buffer_2.data() + buffer_offset);
              }
            }

            if (!preserve_id) {
              for (uint32_t i = static_cast<uint32_t>(buffer_offset);
                   i < buffer_offset + thread_read_count; ++i) {
                std::string read_id;
                read_id.reserve(32);
                read_id.push_back('@');
                read_id.append(std::to_string(num_reads_done + i + 1));
                read_id.push_back('/');
                read_id.append(std::to_string(stream_index + 1));
                id_buffer[i] = std::move(read_id);
              }
            } else if (stream_index == 1 && paired_id_match) {
              for (uint32_t i = static_cast<uint32_t>(buffer_offset);
                   i < buffer_offset + thread_read_count; ++i) {
                modify_id(id_buffer[i], paired_id_code);
              }
            } else {
              const std::string id_member = input_id_paths[stream_index] + "." +
                                            std::to_string(block_num);
              std::string_view id_bytes;
              if (monolithic_id[stream_index]) {
                id_bytes = monolithic_id_blocks[stream_index][block_num];
              } else {
                id_bytes = artifact.require(id_member);
              }
              decompress_id_block_bytes(id_bytes, id_member,
                                        id_buffer.data() + buffer_offset,
                                        thread_read_count, false);
            }
          }
        } catch (...) {
#pragma omp critical
          {
            if (!omp_exception) {
              omp_exception = std::current_exception();
            }
          }
        }
      }

      if (omp_exception) {
        std::rethrow_exception(omp_exception);
      }

      std::string *read_buffer_ptr =
          (stream_index == 0) ? read_buffer_1.data() : read_buffer_2.data();
      std::string *quality_buffer_ptr =
          preserve_quality ? quality_buffer.data() : nullptr;

      if (index_id_suffix_reconstructed && stream_index == 0) {
        for (uint32_t i = 0; i < num_reads_cur_step; ++i) {
          append_grouped_index_suffix_to_id(id_buffer[i], read_buffer_1[i],
                                            paired_end ? &read_buffer_2[i]
                                                       : nullptr);
        }
      }

      if (cb_prefix_stripped && stream_index == 0) {
        for (uint32_t i = 0; i < num_reads_cur_step; ++i) {
          if (cb_seq_cursor + cb_prefix_len > cb_seq_bytes.size()) {
            throw std::runtime_error(
                "Corrupt archive: truncated CB prefix sequence stream");
          }
          read_buffer_ptr[i].insert(0, cb_seq_bytes.data() + cb_seq_cursor,
                                    cb_prefix_len);
          cb_seq_cursor += cb_prefix_len;
          read_lengths_buffer_1[i] += cb_prefix_len;

          if (quality_buffer_ptr != nullptr) {
            if (cb_qual_cursor + cb_prefix_len > cb_qual_bytes.size()) {
              throw std::runtime_error(
                  "Corrupt archive: truncated CB prefix quality stream");
            }
            quality_buffer_ptr[i].insert(
                0, cb_qual_bytes.data() + cb_qual_cursor, cb_prefix_len);
            cb_qual_cursor += cb_prefix_len;
          }
        }
      }

      if (poly_at_stripped) {
        memory_cursor &tail_cursor =
            (stream_index == 0) ? *tail_cursor_1 : *tail_cursor_2;
        for (uint32_t i = 0; i < num_reads_cur_step; ++i) {
          const uint16_t tail_info = tail_cursor.read<uint16_t>("tail info");
          std::string tail_qual;
          const uint32_t tail_len = tail_info >> 1;
          if (tail_len > 0) {
            tail_qual.resize(tail_len);
            tail_cursor.read_bytes(tail_qual.data(), tail_len, "tail quality");
          }
          append_tail(read_buffer_ptr[i],
                      quality_buffer_ptr ? &quality_buffer_ptr[i] : nullptr,
                      tail_info, tail_len > 0 ? &tail_qual : nullptr);
        }
      }

      if (atac_adapter_stripped) {
        memory_cursor &adapter_cursor =
            (stream_index == 0) ? *adapter_cursor_1 : *adapter_cursor_2;
        for (uint32_t i = 0; i < num_reads_cur_step; ++i) {
          const uint8_t adapter_info =
              adapter_cursor.read<uint8_t>("adapter info");
          std::string adapter_qual;
          const uint32_t strip_len = adapter_info >> 1;
          if (strip_len > 0) {
            adapter_qual.resize(strip_len);
            adapter_cursor.read_bytes(adapter_qual.data(), strip_len,
                                      "adapter quality");
          }
          append_atac_adapter_tail(
              read_buffer_ptr[i],
              quality_buffer_ptr ? &quality_buffer_ptr[i] : nullptr,
              adapter_info, strip_len > 0 ? &adapter_qual : nullptr);
        }
      }

      sink.consume_step(id_buffer.data(), read_buffer_ptr, quality_buffer_ptr,
                        num_reads_cur_step, stream_index);
    }

    num_reads_done += num_reads_cur_step;
    if (auto *progress = ProgressBar::GlobalInstance()) {
      progress->update(static_cast<float>(num_reads_done) / num_reads);
    }
    num_blocks_done += archive_encoding_thread_count;
  }

  if (cb_prefix_stripped) {
    if (cb_seq_cursor != cb_seq_bytes.size()) {
      throw std::runtime_error(
          "Corrupt archive: trailing CB prefix sequence bytes remain");
    }
    if (preserve_quality && cb_qual_cursor != cb_qual_bytes.size()) {
      throw std::runtime_error(
          "Corrupt archive: trailing CB prefix quality bytes remain");
    }
  }
  SPRING_LOG_DEBUG("decompress_short complete: total_reads_done=" +
                   std::to_string(num_reads_done));
}

void decompress_long(const decompression_archive_artifact &artifact,
                     DecompressionSink &sink, compression_params &cp,
                     int decoding_num_thr) {
  (void)decoding_num_thr;
  SPRING_LOG_DEBUG(
      "decompress_long start: scratch_dir=" + artifact.scratch_dir +
      ", num_reads=" + std::to_string(cp.read_info.num_reads) +
      ", paired_end=" + std::string(cp.encoding.paired_end ? "true" : "false") +
      ", preserve_id=" +
      std::string(cp.encoding.preserve_id ? "true" : "false") +
      ", preserve_quality=" +
      std::string(cp.encoding.preserve_quality ? "true" : "false"));

  const std::array<std::string, 2> input_read_paths = {"read_1", "read_2"};
  const std::array<std::string, 2> input_quality_paths = {"quality_1",
                                                          "quality_2"};
  const std::array<std::string, 2> input_id_paths = {"id_1", "id_2"};
  const std::array<std::string, 2> input_read_length_paths = {"readlength_1",
                                                              "readlength_2"};

  const uint32_t num_reads = cp.read_info.num_reads;
  const uint8_t paired_id_code = cp.read_info.paired_id_code;
  const bool paired_id_match = cp.read_info.paired_id_match;
  const uint32_t num_reads_per_block = cp.encoding.num_reads_per_block_long;
  const bool paired_end = cp.encoding.paired_end;
  const bool preserve_id = cp.encoding.preserve_id;
  const bool preserve_quality = cp.encoding.preserve_quality;
  const int archive_encoding_thread_count =
      resolve_archive_encoding_thread_count(cp);

  const uint64_t num_reads_per_step =
      compute_num_reads_per_step(num_reads, num_reads_per_block,
                                 archive_encoding_thread_count, paired_end);

  std::vector<std::string> read_buffer(static_cast<size_t>(num_reads_per_step));
  std::vector<std::string> id_buffer(static_cast<size_t>(num_reads_per_step));
  std::vector<std::string> quality_buffer;
  if (preserve_quality) {
    quality_buffer.resize(static_cast<size_t>(num_reads_per_step));
  }
  std::vector<uint32_t> read_lengths_buffer(
      static_cast<size_t>(num_reads_per_step));

  omp_set_num_threads(archive_encoding_thread_count);

  uint32_t num_blocks_done = 0;
  uint32_t num_reads_done = 0;
  for (;;) {
    const uint32_t num_reads_cur_step = compute_num_reads_cur_step(
        num_reads, num_reads_done, num_reads_per_step, paired_end);
    if (num_reads_cur_step == 0) {
      break;
    }
    SPRING_LOG_DEBUG(make_decompress_step_log_message(
        "decompress_long step: num_reads_done=", num_reads_done,
        num_reads_cur_step, num_blocks_done));

    for (int stream_index = 0; stream_index < 2; ++stream_index) {
      if (stream_index == 1 && !paired_end) {
        continue;
      }

      std::exception_ptr omp_exception;
#pragma omp parallel
      {
        try {
          const uint64_t thread_id = omp_get_thread_num();
          if (thread_id * num_reads_per_block < num_reads_cur_step) {
            const uint32_t thread_read_count = compute_thread_read_count(
                num_reads_cur_step, num_reads_per_block, thread_id);
            const uint64_t buffer_offset = thread_id * num_reads_per_block;
            const uint32_t block_num =
                num_blocks_done + static_cast<uint32_t>(thread_id);

            const std::vector<char> read_length_bytes =
                decompress_archive_bsc_member(
                    artifact,
                    compressed_block_file_path(
                        input_read_length_paths[stream_index], block_num),
                    true);
            memory_cursor read_length_cursor(read_length_bytes);
            for (uint32_t read_index = 0; read_index < thread_read_count;
                 ++read_index) {
              read_lengths_buffer[buffer_offset + read_index] =
                  read_length_cursor.read<uint32_t>("read length block");
            }

            const std::vector<char> read_bytes = decompress_archive_bsc_member(
                artifact, compressed_block_file_path(
                              input_read_paths[stream_index], block_num));
            memory_cursor read_cursor(read_bytes);
            for (uint32_t read_index = 0; read_index < thread_read_count;
                 ++read_index) {
              const uint32_t absolute_index =
                  static_cast<uint32_t>(buffer_offset) + read_index;
              read_buffer[absolute_index].resize(
                  read_lengths_buffer[absolute_index]);
              read_cursor.read_bytes(read_buffer[absolute_index].data(),
                                     read_lengths_buffer[absolute_index],
                                     "long read block");
            }

            if (preserve_quality) {
              const std::vector<char> quality_bytes =
                  decompress_archive_bsc_member(
                      artifact,
                      block_file_path(input_quality_paths[stream_index],
                                      block_num));
              memory_cursor quality_cursor(quality_bytes);
              for (uint32_t read_index = 0; read_index < thread_read_count;
                   ++read_index) {
                const uint32_t absolute_index =
                    static_cast<uint32_t>(buffer_offset) + read_index;
                quality_buffer[absolute_index].resize(
                    read_lengths_buffer[absolute_index]);
                quality_cursor.read_bytes(quality_buffer[absolute_index].data(),
                                          read_lengths_buffer[absolute_index],
                                          "quality block");
              }
            }

            if (!preserve_id) {
              for (uint32_t i = static_cast<uint32_t>(buffer_offset);
                   i < buffer_offset + thread_read_count; ++i) {
                std::string read_id;
                read_id.reserve(32);
                read_id.push_back('@');
                read_id.append(std::to_string(num_reads_done + i + 1));
                read_id.push_back('/');
                read_id.append(std::to_string(stream_index + 1));
                id_buffer[i] = std::move(read_id);
              }
            } else if (stream_index == 1 && paired_id_match) {
              for (uint32_t i = static_cast<uint32_t>(buffer_offset);
                   i < buffer_offset + thread_read_count; ++i) {
                modify_id(id_buffer[i], paired_id_code);
              }
            } else {
              const std::string raw_member =
                  block_file_path(input_id_paths[stream_index], block_num);
              const std::string compressed_member = raw_member + ".bsc";
              if (artifact.contains(compressed_member)) {
                decompress_id_block_bytes(
                    artifact.require(compressed_member), compressed_member,
                    id_buffer.data() + buffer_offset, thread_read_count, false);
              } else {
                decompress_id_block_bytes(
                    artifact.require(raw_member), raw_member,
                    id_buffer.data() + buffer_offset, thread_read_count, true);
              }
            }
          }
        } catch (...) {
#pragma omp critical
          {
            if (!omp_exception) {
              omp_exception = std::current_exception();
            }
          }
        }
      }

      if (omp_exception) {
        std::rethrow_exception(omp_exception);
      }

      sink.consume_step(id_buffer.data(), read_buffer.data(),
                        preserve_quality ? quality_buffer.data() : nullptr,
                        num_reads_cur_step, stream_index);
    }

    num_reads_done += num_reads_cur_step;
    if (auto *progress = ProgressBar::GlobalInstance()) {
      progress->update(static_cast<float>(num_reads_done) / num_reads);
    }
    num_blocks_done += archive_encoding_thread_count;
  }

  SPRING_LOG_DEBUG("decompress_long complete: total_reads_done=" +
                   std::to_string(num_reads_done));
}

std::vector<std::string> decompress_unpack_seq_chunks(
    const decompression_archive_artifact &artifact,
    const std::string &packed_seq_base_path, int encoding_thread_count,
    int decoding_thread_count, const compression_params &cp) {
  SPRING_LOG_DEBUG(
      "decompress_unpack_seq start: base_path=" + packed_seq_base_path +
      ", encoding_threads=" + std::to_string(encoding_thread_count) +
      ", decoding_threads=" + std::to_string(decoding_thread_count));
  const std::string monolithic_compressed_path = packed_seq_base_path + ".bsc";

  if (artifact.contains(monolithic_compressed_path) &&
      artifact.require(monolithic_compressed_path).empty()) {
    SPRING_LOG_DEBUG("Skipping sequence unpack: monolithic archive is empty.");
    return std::vector<std::string>(static_cast<size_t>(encoding_thread_count));
  }

  // Decompress the monolithic archive block into raw packed sequence bytes.
  std::vector<char> monolithic_packed_bytes =
      decompress_archive_bsc_member(artifact, monolithic_compressed_path);

  // Slice the monolithic raw bytes into the parallelized chunks expected by
  // callers.
  size_t monolithic_cursor = 0;

  if (std::cmp_greater(encoding_thread_count,
                       compression_params::ReadMetadata::kFileLenThrSize)) {
    throw std::runtime_error(
        std::string("Archive indicates too many sequence chunks "
                    "(encoding_thread_count=") +
        std::to_string(encoding_thread_count) + ") for metadata array size (" +
        std::to_string(compression_params::ReadMetadata::kFileLenThrSize) +
        "). Increase array size in params.h or recreate archive.");
  }

  std::vector<std::vector<char>> packed_chunks(
      static_cast<size_t>(encoding_thread_count));
  for (int tid = 0; tid < encoding_thread_count; tid++) {
    uint64_t chunk_bytes = 0;
    if (monolithic_cursor + sizeof(uint64_t) > monolithic_packed_bytes.size()) {
      throw std::runtime_error(
          "Corrupt archive: failed reading packed chunk size header for tid=" +
          std::to_string(tid));
    }
    std::memcpy(&chunk_bytes,
                monolithic_packed_bytes.data() + monolithic_cursor,
                sizeof(uint64_t));
    monolithic_cursor += sizeof(uint64_t);
    SPRING_LOG_DEBUG("Slicing chunk tid=" + std::to_string(tid) +
                     ", packed_size=" + std::to_string(chunk_bytes));
    if (monolithic_cursor + chunk_bytes > monolithic_packed_bytes.size()) {
      throw std::runtime_error(
          "Corrupt archive: truncated packed sequence stream while slicing "
          "monolithic data");
    }
    packed_chunks[static_cast<size_t>(tid)].assign(
        monolithic_packed_bytes.data() + monolithic_cursor,
        monolithic_packed_bytes.data() + monolithic_cursor +
            static_cast<size_t>(chunk_bytes));
    monolithic_cursor += static_cast<size_t>(chunk_bytes);
  }
  if (monolithic_cursor != monolithic_packed_bytes.size()) {
    throw std::runtime_error(
        "Corrupt archive: trailing bytes after packed sequence chunks.");
  }
  SPRING_LOG_DEBUG(
      "decompress_unpack_seq slicing complete; starting per-chunk decode.");

  std::exception_ptr decode_exception;
  std::vector<std::string> decoded_chunks(
      static_cast<size_t>(encoding_thread_count));
#pragma omp parallel
  {
    const int thread_id = omp_get_thread_num();
    const thread_range range = split_thread_range(
        encoding_thread_count, thread_id, decoding_thread_count);
    for (uint64_t encoding_thread_id = range.begin;
         encoding_thread_id < range.end; encoding_thread_id++) {
      try {
        decoded_chunks[static_cast<size_t>(encoding_thread_id)] =
            decode_packed_sequence_chunk_bytes(
                packed_chunks[static_cast<size_t>(encoding_thread_id)],
                static_cast<int>(encoding_thread_id),
                cp.read_info.file_len_seq_thr[encoding_thread_id],
                cp.encoding.bisulfite_ternary);
      } catch (...) {
#pragma omp critical
        {
          decode_exception = std::current_exception();
        }
      }
    }
  }
  if (decode_exception) {
    std::rethrow_exception(decode_exception);
  }
  SPRING_LOG_DEBUG("decompress_unpack_seq complete.");
  return decoded_chunks;
}

void set_dec_noise_array(std::array<std::array<char, 128>, 128> &dec_noise) {
  dec_noise[(uint8_t)'A'][(uint8_t)'0'] = 'C';
  dec_noise[(uint8_t)'A'][(uint8_t)'1'] = 'G';
  dec_noise[(uint8_t)'A'][(uint8_t)'2'] = 'T';
  dec_noise[(uint8_t)'A'][(uint8_t)'3'] = 'N';
  dec_noise[(uint8_t)'C'][(uint8_t)'0'] = 'A';
  dec_noise[(uint8_t)'C'][(uint8_t)'1'] = 'G';
  dec_noise[(uint8_t)'C'][(uint8_t)'2'] = 'T';
  dec_noise[(uint8_t)'C'][(uint8_t)'3'] = 'N';
  dec_noise[(uint8_t)'G'][(uint8_t)'0'] = 'T';
  dec_noise[(uint8_t)'G'][(uint8_t)'1'] = 'A';
  dec_noise[(uint8_t)'G'][(uint8_t)'2'] = 'C';
  dec_noise[(uint8_t)'G'][(uint8_t)'3'] = 'N';
  dec_noise[(uint8_t)'T'][(uint8_t)'0'] = 'G';
  dec_noise[(uint8_t)'T'][(uint8_t)'1'] = 'C';
  dec_noise[(uint8_t)'T'][(uint8_t)'2'] = 'A';
  dec_noise[(uint8_t)'T'][(uint8_t)'3'] = 'N';
  dec_noise[(uint8_t)'N'][(uint8_t)'0'] = 'A';
  dec_noise[(uint8_t)'N'][(uint8_t)'1'] = 'G';
  dec_noise[(uint8_t)'N'][(uint8_t)'2'] = 'C';
  dec_noise[(uint8_t)'N'][(uint8_t)'3'] = 'T';
}

} // namespace spring
