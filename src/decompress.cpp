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
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
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
  std::string path;
#ifndef _WIN32
  MmapView mmap;
#endif
  std::string owned_data;
  const char *data = nullptr;
};

std::runtime_error file_error(const std::string &prefix,
                              const std::string &path) {
  return std::runtime_error(prefix + ": " + path + ": " +
                            std::string(strerror(errno)));
}

void open_reference_chunk(reference_chunk &chunk) {
#ifdef _WIN32
  if (chunk.size == 0) {
    chunk.owned_data.clear();
    chunk.data = nullptr;
    return;
  }

  std::ifstream input(chunk.path, std::ios::binary);
  if (!input.is_open()) {
    throw file_error("Error opening decoded reference chunk", chunk.path);
  }

  chunk.owned_data.resize(static_cast<size_t>(chunk.size));
  input.read(chunk.owned_data.data(),
             static_cast<std::streamsize>(chunk.owned_data.size()));
  if (!input ||
      input.gcount() != static_cast<std::streamsize>(chunk.owned_data.size())) {
    throw std::runtime_error("Error reading decoded reference chunk: " +
                             chunk.path);
  }

  chunk.data = chunk.owned_data.data();
#else
  try {
    chunk.mmap.mapFromPath(chunk.path, static_cast<size_t>(chunk.size));
    chunk.data = chunk.mmap.data();
  } catch (const std::exception &e) {
    throw std::runtime_error(
        std::string("Error mapping decoded reference chunk: ") + e.what());
  }
#endif
}

void close_reference_chunk(reference_chunk &chunk) {
#ifdef _WIN32
  chunk.owned_data.clear();
  chunk.data = nullptr;
#else
  chunk.mmap.unmap();
  chunk.data = nullptr;
#endif
};

class reference_sequence_store {
public:
  reference_sequence_store(const reference_sequence_store &) = delete;
  reference_sequence_store &
  operator=(const reference_sequence_store &) = delete;
  reference_sequence_store(reference_sequence_store &&) = delete;
  reference_sequence_store &operator=(reference_sequence_store &&) = delete;

  reference_sequence_store(const std::string &packed_seq_path,
                           const int encoding_thread_count,
                           const int decode_thread_count,
                           const compression_params &cp) {
    decompress_unpack_seq(packed_seq_path, encoding_thread_count,
                          decode_thread_count, cp);

    chunks_.reserve(static_cast<size_t>(encoding_thread_count));
    uint64_t next_start_offset = 0;
    for (int encoding_thread_id = 0; encoding_thread_id < encoding_thread_count;
         encoding_thread_id++) {
      reference_chunk chunk;
      chunk.path = packed_seq_path + '.' + std::to_string(encoding_thread_id);

      chunk.size = cp.read_info.file_len_seq_thr[encoding_thread_id];
      chunk.start_offset = next_start_offset;
      next_start_offset += chunk.size;

      open_reference_chunk(chunk);
      start_offsets_.push_back(chunk.start_offset);
      chunks_.push_back(std::move(chunk));
      total_size_ = next_start_offset;
    }
  }

  ~reference_sequence_store() {
    for (reference_chunk &chunk : chunks_) {
      close_reference_chunk(chunk);
      safe_remove_file(chunk.path);
    }
  }

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
  return base_path + '.' + std::to_string(block_num);
}

std::string compressed_block_file_path(const std::string &base_path,
                                       const uint32_t block_num) {
  return block_file_path(base_path, block_num) + ".bsc";
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
} // namespace

void write_fastq_block(std::ostream &output_stream, std::string *id_buffer,
                       std::string *read_buffer,
                       const std::string *quality_array,
                       uint32_t output_read_count, int num_thr,
                       bool gzip_output, bool bgzf_output,
                       int compression_level, bool use_crlf, bool fasta_mode);

FileDecompressionSink::FileDecompressionSink(const std::string &outfile_1,
                                             const std::string &outfile_2,
                                             const compression_params &cp,
                                             bool crlf, const bool (&gzip)[2],
                                             const bool (&bgzf)[2])
    : use_crlf(crlf), fasta_mode(cp.encoding.fasta_mode),
      compression_level(cp.encoding.compression_level),
      num_thr(cp.encoding.num_thr), paired_end(cp.encoding.paired_end) {
  should_gzip[0] = gzip[0];
  should_gzip[1] = gzip[1];
  should_bgzf[0] = bgzf[0];
  should_bgzf[1] = bgzf[1];

  output_streams[0].open(outfile_1, std::ios::binary);
  if (!output_streams[0])
    throw std::runtime_error("Failed to open output file: " + outfile_1);
  if (paired_end) {
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
  write_fastq_block(output_streams[stream_index], id_buffer, read_buffer,
                    quality_buffer, count, num_thr, should_gzip[stream_index],
                    should_bgzf[stream_index], compression_level, use_crlf,
                    fasta_mode);
}

void write_step_output(std::ofstream &output_stream, std::string *id_buffer,
                       std::string *read_buffer,
                       const std::string *quality_array,
                       const uint32_t output_read_count, const int num_thr,
                       const bool gzip_output, const bool bgzf_output,
                       const int compression_level, const bool use_crlf,
                       const bool fasta_mode) {
  write_fastq_block(output_stream, id_buffer, read_buffer, quality_array,
                    output_read_count, num_thr, gzip_output, bgzf_output,
                    compression_level, use_crlf, fasta_mode);
}

void decode_packed_sequence_chunk(const std::string &packed_seq_base_path,
                                  const int encoding_thread_id,
                                  const uint64_t num_bases,
                                  bool methyl_ternary) {
  SPRING_LOG_DEBUG("decode_packed_sequence_chunk start: chunk=" +
                   std::to_string(encoding_thread_id) +
                   ", num_bases=" + std::to_string(num_bases) +
                   ", ternary=" + (methyl_ternary ? "yes" : "no"));
  const std::string chunk_base_path =
      packed_seq_base_path + '.' + std::to_string(encoding_thread_id);
  const std::string temporary_output_path = chunk_base_path + ".tmp";
  static const char base_lookup[4] = {'A', 'G', 'C', 'T'};

  std::ofstream unpacked_output(temporary_output_path, std::ios::binary);
  if (!unpacked_output.is_open())
    throw std::runtime_error("Cannot open temporary output: " +
                             temporary_output_path);
  std::ifstream packed_input(chunk_base_path, std::ios::binary);
  if (!packed_input.is_open())
    throw std::runtime_error("Cannot open packed input: " + chunk_base_path);

  std::vector<char> unpacked_buffer;
  unpacked_buffer.reserve(1U << 18);
  uint64_t bases_decoded = 0;

  if (!methyl_ternary) {
    std::vector<uint8_t> packed_buffer(1U << 16);
    while (packed_input && bases_decoded < num_bases) {
      packed_input.read(reinterpret_cast<char *>(packed_buffer.data()),
                        static_cast<std::streamsize>(packed_buffer.size()));
      const std::streamsize packed_bytes_read = packed_input.gcount();
      if (packed_bytes_read <= 0)
        break;

      unpacked_buffer.clear();
      for (std::streamsize packed_index = 0; packed_index < packed_bytes_read;
           packed_index++) {
        uint8_t byte = packed_buffer[packed_index];
        for (int i = 0; i < 4 && bases_decoded < num_bases; i++) {
          unpacked_buffer.push_back(base_lookup[byte & 3]);
          byte >>= 2;
          bases_decoded++;
        }
      }
      unpacked_output.write(
          unpacked_buffer.data(),
          static_cast<std::streamsize>(unpacked_buffer.size()));
    }
  } else {
    while (packed_input && bases_decoded < num_bases) {
      uint8_t byte;
      if (!packed_input.read(reinterpret_cast<char *>(&byte), 1))
        break;

      if (byte < 243) {
        // Standard ternary: 5 bases per byte
        for (int k = 0; k < 5 && bases_decoded < num_bases; k++) {
          uint8_t val = byte % 3;
          byte /= 3;
          static constexpr char kBases[3] = {'A', 'G', 'T'};
          unpacked_output.put(kBases[val]);
          bases_decoded++;
        }
      } else {
        // Escape: 2 bits per base, 5 bases in uint16_t
        uint16_t escape;
        if (!packed_input.read(reinterpret_cast<char *>(&escape),
                               sizeof(uint16_t)))
          break;
        for (int k = 0; k < 5 && bases_decoded < num_bases; k++) {
          uint8_t val = (escape >> (2 * k)) & 3;
          unpacked_output.put(base_lookup[val]);
          bases_decoded++;
        }
      }
    }
  }

  packed_input.close();
  unpacked_output.close();
  SPRING_LOG_DEBUG("decode_packed_sequence_chunk done: chunk=" +
                   std::to_string(encoding_thread_id) +
                   ", bases=" + std::to_string(bases_decoded));
  safe_remove_file(chunk_base_path);
  if (!safe_rename_file(temporary_output_path, chunk_base_path))
    throw std::runtime_error("Failed to rename decoded chunk " +
                             std::to_string(encoding_thread_id));
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

bool decompress_and_slice_id(const std::string &temp_path_bsc,
                             const std::string &base_id_path,
                             const uint64_t *file_len_thr,
                             const uint32_t num_reads,
                             const uint32_t num_reads_per_block) {
  const std::string packed_path = base_id_path + ".packed";
  if (!std::filesystem::exists(temp_path_bsc))
    return false;

  safe_bsc_decompress(temp_path_bsc, packed_path);

  std::ifstream packed_in(packed_path, std::ios::binary);
  if (!packed_in)
    throw std::runtime_error("Failed to open packed ID file for slicing.");

  const uint32_t num_blocks =
      (num_reads + num_reads_per_block - 1) / num_reads_per_block;
  if (num_blocks > compression_params::ReadMetadata::kFileLenThrSize) {
    throw std::runtime_error(
        std::string("Archive contains too many ID blocks (") +
        std::to_string(num_blocks) + ") for metadata array size (" +
        std::to_string(compression_params::ReadMetadata::kFileLenThrSize) +
        "). Increase array size in params.h.");
  }
  for (uint32_t b = 0; b < num_blocks; b++) {
    const uint64_t block_len = file_len_thr[b];
    if (block_len == 0)
      continue;

    const std::string block_path = base_id_path + "." + std::to_string(b);
    std::ofstream block_out(block_path, std::ios::binary);
    if (!block_out)
      throw std::runtime_error("Failed to open ID block file for slicing.");

    std::vector<char> buffer(block_len);
    packed_in.read(buffer.data(), block_len);
    block_out.write(buffer.data(), block_len);
  }
  packed_in.close();
  std::filesystem::remove(packed_path);
  return true;
}

void set_dec_noise_array(std::array<std::array<char, 128>, 128> &dec_noise);

void decompress_short(const std::string &temp_dir, DecompressionSink &sink,
                      compression_params &cp, int decoding_num_thr) {
  SPRING_LOG_DEBUG(
      "decompress_short start: temp_dir=" + temp_dir +
      ", num_reads=" + std::to_string(cp.read_info.num_reads) +
      ", paired_end=" + std::string(cp.encoding.paired_end ? "true" : "false") +
      ", preserve_order=" +
      std::string(cp.encoding.preserve_order ? "true" : "false") +
      ", preserve_id=" +
      std::string(cp.encoding.preserve_id ? "true" : "false") +
      ", preserve_quality=" +
      std::string(cp.encoding.preserve_quality ? "true" : "false"));
  std::string base_dir = temp_dir;

  std::string file_seq = base_dir + "/read_seq.bin";
  std::string file_flag = base_dir + "/read_flag.txt";
  std::string file_pos = base_dir + "/read_pos.bin";
  std::string file_pos_pair = base_dir + "/read_pos_pair.bin";
  std::string file_RC = base_dir + "/read_rev.txt";
  std::string file_RC_pair = base_dir + "/read_rev_pair.txt";
  std::string file_readlength = base_dir + "/read_lengths.bin";
  std::string file_unaligned = base_dir + "/read_unaligned.txt";
  std::string file_noise = base_dir + "/read_noise.txt";
  std::string file_noisepos = base_dir + "/read_noisepos.bin";
  std::string input_quality_paths[2];
  std::string input_id_paths[2];

  input_quality_paths[0] = base_dir + "/quality_1";
  input_quality_paths[1] = base_dir + "/quality_2";
  input_id_paths[0] = base_dir + "/id_1";
  input_id_paths[1] = base_dir + "/id_2";

  bool monolithic_id[2] = {false, false};
  if (cp.encoding.preserve_id) {
    const uint32_t file_read_count = (cp.encoding.paired_end)
                                         ? (cp.read_info.num_reads / 2)
                                         : cp.read_info.num_reads;
    monolithic_id[0] =
        decompress_and_slice_id(input_id_paths[0] + ".bsc", input_id_paths[0],
                                cp.read_info.file_len_id_thr, file_read_count,
                                cp.encoding.num_reads_per_block);
    if (cp.encoding.paired_end && !cp.read_info.paired_id_match) {
      monolithic_id[1] =
          decompress_and_slice_id(input_id_paths[1] + ".bsc", input_id_paths[1],
                                  cp.read_info.file_len_id_thr, file_read_count,
                                  cp.encoding.num_reads_per_block);
    }
  }
  SPRING_LOG_DEBUG("decompress_short ID block mode: stream1_monolithic=" +
                   std::string(monolithic_id[0] ? "true" : "false") +
                   ", stream2_monolithic=" +
                   std::string(monolithic_id[1] ? "true" : "false"));

  uint32_t num_reads = cp.read_info.num_reads;
  uint8_t paired_id_code = cp.read_info.paired_id_code;
  bool paired_id_match = cp.read_info.paired_id_match;
  uint32_t num_reads_per_block = cp.encoding.num_reads_per_block;
  bool paired_end = cp.encoding.paired_end;
  bool preserve_id = cp.encoding.preserve_id;
  bool preserve_quality = cp.encoding.preserve_quality;
  bool preserve_order = cp.encoding.preserve_order;
  const bool poly_at_stripped = cp.encoding.poly_at_stripped;
  const int archive_encoding_thread_count =
      resolve_archive_encoding_thread_count(cp);

  // Open tail files if poly-A/T stripping was used during compression.
  std::ifstream f_tail_1, f_tail_2;
  if (poly_at_stripped) {
    f_tail_1.open(temp_dir + "/tail_1.bin", std::ios::binary);
    if (!f_tail_1)
      throw std::runtime_error("poly_at_stripped set but tail_1.bin not found");
    if (paired_end) {
      f_tail_2.open(temp_dir + "/tail_2.bin", std::ios::binary);
      if (!f_tail_2)
        throw std::runtime_error(
            "poly_at_stripped set but tail_2.bin not found");
    }
  }

  const uint64_t num_reads_per_step =
      compute_num_reads_per_step(num_reads, num_reads_per_block,
                                 archive_encoding_thread_count, paired_end);

  std::vector<std::string> read_buffer_1(
      static_cast<size_t>(num_reads_per_step));
  std::vector<std::string> read_buffer_2;
  if (paired_end)
    read_buffer_2.resize(static_cast<size_t>(num_reads_per_step));
  std::vector<std::string> id_buffer(static_cast<size_t>(num_reads_per_step));
  std::vector<std::string> quality_buffer;
  if (preserve_quality)
    quality_buffer.resize(static_cast<size_t>(num_reads_per_step));
  std::vector<uint32_t> read_lengths_buffer_1(
      static_cast<size_t>(num_reads_per_step));
  std::vector<uint32_t> read_lengths_buffer_2;
  if (paired_end)
    read_lengths_buffer_2.resize(static_cast<size_t>(num_reads_per_step));
  std::array<std::array<char, 128>, 128> decoded_noise_table;
  set_dec_noise_array(decoded_noise_table);

  omp_set_num_threads(decoding_num_thr);

  // Rebuild the packed reference sequence once before block processing.
  reference_sequence_store seq(file_seq, archive_encoding_thread_count,
                               decoding_num_thr, cp);

  bool done = false;
  uint32_t num_blocks_done = 0;
  uint32_t num_reads_done = 0;
  while (!done) {
    uint32_t num_reads_cur_step = compute_num_reads_cur_step(
        num_reads, num_reads_done, num_reads_per_step, paired_end);
    if (num_reads_cur_step == 0)
      break;
    SPRING_LOG_DEBUG("decompress_short step: num_reads_done=" +
                     std::to_string(num_reads_done) +
                     ", reads_this_step=" + std::to_string(num_reads_cur_step) +
                     ", num_blocks_done=" + std::to_string(num_blocks_done));
    for (int stream_index = 0; stream_index < 2; stream_index++) {
      if (stream_index == 1 && !paired_end)
        continue;
      std::exception_ptr omp_exception;
#pragma omp parallel
      {
        try {
          uint64_t thread_id = omp_get_thread_num();
          if (thread_id * num_reads_per_block < num_reads_cur_step) {
            const uint32_t thread_read_count = compute_thread_read_count(
                num_reads_cur_step, num_reads_per_block, thread_id);
            const uint64_t buffer_offset = thread_id * num_reads_per_block;

            if (stream_index == 0) {
              // Read decompression done when stream_index = 0 (even for PE)
              const uint32_t block_num = num_blocks_done + thread_id;

              decompress_bsc_block(file_flag, block_num);
              decompress_bsc_block(file_pos, block_num);
              decompress_bsc_block(file_noise, block_num);
              decompress_bsc_block(file_noisepos, block_num);
              decompress_bsc_block(file_unaligned, block_num);
              decompress_bsc_block(file_readlength, block_num);
              decompress_bsc_block(file_RC, block_num);

              if (paired_end) {
                decompress_bsc_block(file_pos_pair, block_num);
                decompress_bsc_block(file_RC_pair, block_num);
              }

              // Read streams are shared between mates, so decode them once when
              // handling the first output stream.
              std::ifstream f_flag(block_file_path(file_flag, block_num),
                                   std::ios::binary);
              std::ifstream f_noise(block_file_path(file_noise, block_num),
                                    std::ios::binary);
              std::ifstream f_noisepos(
                  block_file_path(file_noisepos, block_num), std::ios::binary);
              std::ifstream f_pos(block_file_path(file_pos, block_num),
                                  std::ios::binary);
              std::ifstream f_RC(block_file_path(file_RC, block_num),
                                 std::ios::binary);
              std::ifstream f_unaligned(
                  block_file_path(file_unaligned, block_num), std::ios::binary);
              std::ifstream f_readlength(
                  block_file_path(file_readlength, block_num),
                  std::ios::binary);
              std::ifstream f_pos_pair;
              std::ifstream f_RC_pair;
              if (paired_end) {
                f_pos_pair.open(block_file_path(file_pos_pair, block_num),
                                std::ios::binary);
                f_RC_pair.open(block_file_path(file_RC_pair, block_num));
              }

              char read_flag;
              uint64_t read_1_position;
              uint64_t read_2_position;
              uint64_t previous_position;
              bool read_1_is_singleton;
              bool read_2_is_singleton;
              char read_1_orientation;
              char read_2_orientation;
              uint16_t read_length;
              uint16_t position_delta_16;
              bool first_read_of_block = true;
              for (uint32_t i = buffer_offset;
                   i < buffer_offset + thread_read_count; i++) {
                f_flag >> read_flag;
                f_readlength.read(byte_ptr(&read_length), sizeof(uint16_t));
                read_lengths_buffer_1[i] = read_length;
                read_1_is_singleton = (read_flag == '2') || (read_flag == '4');
                if (!read_1_is_singleton) {
                  if (preserve_order) {
                    f_pos.read(byte_ptr(&read_1_position), sizeof(uint64_t));
                    if (!f_pos)
                      throw std::runtime_error(
                          "Corrupt archive: failed reading position");
                  } else {
                    if (first_read_of_block) {
                      // Non-order-preserving mode stores the first absolute
                      // position in each block and then deltas afterward.
                      first_read_of_block = false;
                      f_pos.read(byte_ptr(&read_1_position), sizeof(uint64_t));
                      if (!f_pos)
                        throw std::runtime_error(
                            "Corrupt archive: failed reading first position of "
                            "block");
                      previous_position = read_1_position;
                    } else {
                      f_pos.read(byte_ptr(&position_delta_16),
                                 sizeof(uint16_t));
                      if (!f_pos)
                        throw std::runtime_error(
                            "Corrupt archive: failed reading position delta");
                      if (position_delta_16 == 65535) {
                        f_pos.read(byte_ptr(&read_1_position),
                                   sizeof(uint64_t));
                        if (!f_pos)
                          throw std::runtime_error(
                              "Corrupt archive: failed reading fallback 64-bit "
                              "position");
                      } else {
                        read_1_position = previous_position + position_delta_16;
                      }
                      previous_position = read_1_position;
                    }
                  }
                  if (!(f_RC >> read_1_orientation))
                    throw std::runtime_error(
                        "Corrupt archive: failed reading orientation");
                  std::string read =
                      seq.read(read_1_position, read_lengths_buffer_1[i]);
                  std::string noise_codes;
                  uint16_t noise_position_delta;
                  uint16_t previous_noise_position = 0;
                  std::getline(f_noise, noise_codes);
                  for (uint16_t k = 0; k < noise_codes.size(); k++) {
                    f_noisepos.read(byte_ptr(&noise_position_delta),
                                    sizeof(uint16_t));
                    noise_position_delta += previous_noise_position;
                    read[noise_position_delta] =
                        decoded_noise_table[(uint8_t)read[noise_position_delta]]
                                           [(uint8_t)noise_codes[k]];
                    previous_noise_position = noise_position_delta;
                  }
                  if (read_1_orientation == 'd')
                    read_buffer_1[i] = read;
                  else
                    read_buffer_1[i] =
                        reverse_complement(read, read_lengths_buffer_1[i]);
                } else {
                  read_buffer_1[i].resize(read_lengths_buffer_1[i]);
                  f_unaligned.read(&read_buffer_1[i][0],
                                   read_lengths_buffer_1[i]);
                }

                if (paired_end) {
                  int16_t mate_position_delta_16;
                  read_2_is_singleton =
                      (read_flag == '2') || (read_flag == '3');
                  f_readlength.read(byte_ptr(&read_length), sizeof(uint16_t));
                  read_lengths_buffer_2[i] = read_length;
                  if (!read_2_is_singleton) {
                    if (read_flag == '1' || read_flag == '4') {
                      f_pos.read(byte_ptr(&read_2_position), sizeof(uint64_t));
                      if (!f_pos)
                        throw std::runtime_error(
                            "Corrupt archive: failed reading mate 2 position");
                      if (!(f_RC >> read_2_orientation))
                        throw std::runtime_error("Corrupt archive: failed "
                                                 "reading mate 2 orientation");
                    } else {
                      // Mate 2 can be stored relative to mate 1 inside the same
                      // block.
                      char relative_orientation_flag;
                      f_pos_pair.read(byte_ptr(&mate_position_delta_16),
                                      sizeof(int16_t));
                      if (!f_pos_pair)
                        throw std::runtime_error("Corrupt archive: failed "
                                                 "reading mate position delta");
                      const int64_t mate_position_signed =
                          static_cast<int64_t>(read_1_position) +
                          static_cast<int64_t>(mate_position_delta_16);
                      if (mate_position_signed < 0) {
                        throw std::runtime_error(
                            "Corrupt archive: negative mate position");
                      }
                      read_2_position =
                          static_cast<uint64_t>(mate_position_signed);
                      if (!(f_RC_pair >> relative_orientation_flag))
                        throw std::runtime_error(
                            "Corrupt archive: failed reading relative mate "
                            "orientation");
                      if (relative_orientation_flag == '0')
                        read_2_orientation =
                            (read_1_orientation == 'd') ? 'r' : 'd';
                      else
                        read_2_orientation =
                            (read_1_orientation == 'd') ? 'd' : 'r';
                    }
                    std::string read =
                        seq.read(read_2_position, read_lengths_buffer_2[i]);
                    std::string noise_codes;
                    uint16_t noise_position_delta;
                    uint16_t previous_noise_position = 0;
                    std::getline(f_noise, noise_codes);
                    for (uint16_t k = 0; k < noise_codes.size(); k++) {
                      f_noisepos.read(byte_ptr(&noise_position_delta),
                                      sizeof(uint16_t));
                      noise_position_delta += previous_noise_position;
                      read[noise_position_delta] =
                          decoded_noise_table[(
                              uint8_t)read[noise_position_delta]]
                                             [(uint8_t)noise_codes[k]];
                      previous_noise_position = noise_position_delta;
                    }
                    if (read_2_orientation == 'd')
                      read_buffer_2[i] = read;
                    else
                      read_buffer_2[i] =
                          reverse_complement(read, read_lengths_buffer_2[i]);
                  } else {
                    read_buffer_2[i].resize(read_lengths_buffer_2[i]);
                    f_unaligned.read(&read_buffer_2[i][0],
                                     read_lengths_buffer_2[i]);
                  }
                }
              }

              f_flag.close();
              f_noise.close();
              f_noisepos.close();
              f_pos.close();
              f_RC.close();
              f_unaligned.close();
              f_readlength.close();
              if (paired_end) {
                f_pos_pair.close();
                f_RC_pair.close();
              }

              safe_remove_file(block_file_path(file_flag, block_num));
              safe_remove_file(block_file_path(file_pos, block_num));
              safe_remove_file(block_file_path(file_noise, block_num));
              safe_remove_file(block_file_path(file_noisepos, block_num));
              safe_remove_file(block_file_path(file_unaligned, block_num));
              safe_remove_file(block_file_path(file_readlength, block_num));
              safe_remove_file(block_file_path(file_RC, block_num));
              if (paired_end) {
                safe_remove_file(block_file_path(file_pos_pair, block_num));
                safe_remove_file(block_file_path(file_RC_pair, block_num));
              }
            }
            // Decompress ids and quality
            std::string input_path;
            if (preserve_quality) {
              input_path = input_quality_paths[stream_index] + "." +
                           std::to_string(num_blocks_done + thread_id);
              if (stream_index == 0) {
                safe_bsc_str_array_decompress(
                    input_path, quality_buffer.data() + buffer_offset,
                    thread_read_count,
                    read_lengths_buffer_1.data() + buffer_offset);
              } else {
                safe_bsc_str_array_decompress(
                    input_path, quality_buffer.data() + buffer_offset,
                    thread_read_count,
                    read_lengths_buffer_2.data() + buffer_offset);
              }
              safe_remove_file(input_path);
            }
            if (!preserve_id) {
              for (uint32_t i = buffer_offset;
                   i < buffer_offset + thread_read_count; i++) {
                std::string read_id;
                read_id.reserve(32);
                read_id.push_back('@');
                read_id.append(std::to_string(num_reads_done + i + 1));
                read_id.push_back('/');
                read_id.append(std::to_string(stream_index + 1));
                id_buffer[i] = std::move(read_id);
              }
            } else {
              if (stream_index == 1 && paired_id_match) {
                for (uint32_t i = buffer_offset;
                     i < buffer_offset + thread_read_count; i++)
                  modify_id(id_buffer[i], paired_id_code);
              } else {
                input_path = input_id_paths[stream_index] + "." +
                             std::to_string(num_blocks_done + thread_id);
                decompress_id_block(input_path.c_str(),
                                    id_buffer.data() + buffer_offset,
                                    thread_read_count, false);
                safe_remove_file(input_path);
              }
            }
          }
        } catch (...) {
#pragma omp critical
          {
            omp_exception = std::current_exception();
          }
        }
      }
      if (omp_exception) {
        std::rethrow_exception(omp_exception);
      }

      std::string *read_buffer_ptr =
          (stream_index == 0) ? read_buffer_1.data() : read_buffer_2.data();
      const std::string *quality_ptr =
          preserve_quality ? quality_buffer.data() : nullptr;

      // Restore poly-A/T tails before computing integrity CRCs in the sink.
      if (poly_at_stripped) {
        std::ifstream &f_tail = (stream_index == 0) ? f_tail_1 : f_tail_2;
        std::string *quality_buf_ptr =
            preserve_quality ? quality_buffer.data() : nullptr;
        for (uint32_t i = 0; i < num_reads_cur_step; ++i) {
          uint16_t tail_info = 0;
          f_tail.read(reinterpret_cast<char *>(&tail_info), sizeof(uint16_t));
          if (f_tail && tail_info != 0) {
            const uint32_t tail_len = tail_info >> 1;
            std::string tail_qual;
            if (tail_len > 0) {
              tail_qual.resize(tail_len);
              f_tail.read(&tail_qual[0], tail_len);
            }
            append_tail(read_buffer_ptr[i],
                        quality_buf_ptr ? &quality_buf_ptr[i] : nullptr,
                        tail_info, tail_len > 0 ? &tail_qual : nullptr);
          }
        }
      }

      sink.consume_step(id_buffer.data(), read_buffer_ptr, quality_ptr,
                        num_reads_cur_step, stream_index);
    }
    num_reads_done += num_reads_cur_step;
    if (auto *progress = ProgressBar::GlobalInstance()) {
      progress->update(static_cast<float>(num_reads_done) / num_reads);
    }
    num_blocks_done += archive_encoding_thread_count;
  }
  SPRING_LOG_DEBUG("decompress_short complete: total_reads_done=" +
                   std::to_string(num_reads_done));
}

void decompress_long(const std::string &temp_dir, DecompressionSink &sink,
                     compression_params &cp, int decoding_num_thr) {
  SPRING_LOG_DEBUG(
      "decompress_long start: temp_dir=" + temp_dir +
      ", num_reads=" + std::to_string(cp.read_info.num_reads) +
      ", paired_end=" + std::string(cp.encoding.paired_end ? "true" : "false") +
      ", preserve_id=" +
      std::string(cp.encoding.preserve_id ? "true" : "false") +
      ", preserve_quality=" +
      std::string(cp.encoding.preserve_quality ? "true" : "false"));
  std::string input_read_paths[2];
  std::string input_quality_paths[2];
  std::string input_id_paths[2];
  std::string input_read_length_paths[2];
  std::string base_dir = temp_dir;
  input_read_paths[0] = base_dir + "/read_1";
  input_read_paths[1] = base_dir + "/read_2";
  input_quality_paths[0] = base_dir + "/quality_1";
  input_quality_paths[1] = base_dir + "/quality_2";
  input_id_paths[0] = base_dir + "/id_1";
  input_id_paths[1] = base_dir + "/id_2";
  input_read_length_paths[0] = base_dir + "/readlength_1";
  input_read_length_paths[1] = base_dir + "/readlength_2";

  uint32_t num_reads = cp.read_info.num_reads;
  uint8_t paired_id_code = cp.read_info.paired_id_code;
  bool paired_id_match = cp.read_info.paired_id_match;
  uint32_t num_reads_per_block = cp.encoding.num_reads_per_block_long;
  bool paired_end = cp.encoding.paired_end;
  bool preserve_id = cp.encoding.preserve_id;
  bool preserve_quality = cp.encoding.preserve_quality;
  const int archive_encoding_thread_count =
      resolve_archive_encoding_thread_count(cp);

  const uint64_t num_reads_per_step =
      compute_num_reads_per_step(num_reads, num_reads_per_block,
                                 archive_encoding_thread_count, paired_end);

  std::vector<std::string> read_buffer(static_cast<size_t>(num_reads_per_step));
  std::vector<std::string> id_buffer(static_cast<size_t>(num_reads_per_step));
  std::vector<std::string> quality_buffer;
  if (preserve_quality)
    quality_buffer.resize(static_cast<size_t>(num_reads_per_step));
  std::vector<uint32_t> read_lengths_buffer(
      static_cast<size_t>(num_reads_per_step));

  omp_set_num_threads(decoding_num_thr);

  bool done = false;

  uint32_t num_blocks_done = 0;
  uint32_t num_reads_done = 0;
  while (!done) {
    uint32_t num_reads_cur_step = compute_num_reads_cur_step(
        num_reads, num_reads_done, num_reads_per_step, paired_end);
    if (num_reads_cur_step == 0)
      break;
    SPRING_LOG_DEBUG("decompress_long step: num_reads_done=" +
                     std::to_string(num_reads_done) +
                     ", reads_this_step=" + std::to_string(num_reads_cur_step) +
                     ", num_blocks_done=" + std::to_string(num_blocks_done));
    for (int stream_index = 0; stream_index < 2; stream_index++) {
      if (stream_index == 1 && !paired_end)
        continue;
#pragma omp parallel
      {
        uint64_t thread_id = omp_get_thread_num();
        if (thread_id * num_reads_per_block < num_reads_cur_step) {
          const uint32_t thread_read_count = compute_thread_read_count(
              num_reads_cur_step, num_reads_per_block, thread_id);
          const uint64_t buffer_offset = thread_id * num_reads_per_block;
          const uint32_t block_num = num_blocks_done + thread_id;

          // Decompress read lengths file and read into array
          decompress_read_length_block(input_read_length_paths[stream_index],
                                       block_num, read_lengths_buffer.data(),
                                       buffer_offset, thread_read_count);

          std::string block_base_path =
              input_read_paths[stream_index] + "." + std::to_string(block_num);
          const std::string compressed_read_path = block_base_path + ".bsc";
          if (std::filesystem::exists(compressed_read_path)) {
            safe_bsc_decompress(compressed_read_path, block_base_path);
            safe_remove_file(compressed_read_path);
          }
          read_raw_string_block(
              block_base_path, read_buffer.data() + buffer_offset,
              thread_read_count, read_lengths_buffer.data() + buffer_offset);
          safe_remove_file(block_base_path);

          if (preserve_quality) {
            std::string quality_base_path = input_quality_paths[stream_index] +
                                            "." + std::to_string(block_num);
            const std::string quality_raw_path = quality_base_path + ".raw";
            safe_bsc_decompress(quality_base_path, quality_raw_path);
            read_raw_string_block(
                quality_raw_path, quality_buffer.data() + buffer_offset,
                thread_read_count, read_lengths_buffer.data() + buffer_offset);
            safe_remove_file(quality_raw_path);
            safe_remove_file(quality_base_path);
          }
          if (!preserve_id) {
            for (uint32_t i = buffer_offset;
                 i < buffer_offset + thread_read_count; i++) {
              std::string read_id;
              read_id.reserve(32);
              read_id.push_back('@');
              read_id.append(std::to_string(num_reads_done + i + 1));
              read_id.push_back('/');
              read_id.append(std::to_string(stream_index + 1));
              id_buffer[i] = std::move(read_id);
            }
          } else {
            if (stream_index == 1 && paired_id_match) {
              for (uint32_t i = buffer_offset;
                   i < buffer_offset + thread_read_count; i++)
                modify_id(id_buffer[i], paired_id_code);
            } else {
              std::string id_base_path = input_id_paths[stream_index] + "." +
                                         std::to_string(block_num);
              const std::string compressed_id_path = id_base_path + ".bsc";
              if (std::filesystem::exists(compressed_id_path)) {
                decompress_id_block(compressed_id_path.c_str(),
                                    id_buffer.data() + buffer_offset,
                                    thread_read_count, false);
                safe_remove_file(compressed_id_path);
              } else {
                // Backward compatibility: raw packed ID blocks from older
                // long-mode archives.
                decompress_id_block(id_base_path.c_str(),
                                    id_buffer.data() + buffer_offset,
                                    thread_read_count, true);
                safe_remove_file(id_base_path);
              }
            }
          }
        }
      }

      std::string *read_buffer_ptr = read_buffer.data();
      const std::string *quality_ptr =
          preserve_quality ? quality_buffer.data() : nullptr;
      sink.consume_step(id_buffer.data(), read_buffer_ptr, quality_ptr,
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

void decompress_unpack_seq(const std::string &packed_seq_base_path,
                           int encoding_thread_count, int decoding_thread_count,
                           const compression_params &cp) {
  SPRING_LOG_DEBUG(
      "decompress_unpack_seq start: base_path=" + packed_seq_base_path +
      ", encoding_threads=" + std::to_string(encoding_thread_count) +
      ", decoding_threads=" + std::to_string(decoding_thread_count));
  const std::string monolithic_compressed_path = packed_seq_base_path + ".bsc";
  const std::string monolithic_packed_path = packed_seq_base_path + ".packed";

  if (std::filesystem::exists(monolithic_compressed_path) &&
      std::filesystem::file_size(monolithic_compressed_path) == 0) {
    SPRING_LOG_DEBUG("Skipping sequence unpack: monolithic archive is empty.");
    return;
  }

  // Decompress the monolithic archive block into raw packed sequence data.
  safe_bsc_decompress(monolithic_compressed_path, monolithic_packed_path);
  safe_remove_file(monolithic_compressed_path);

  // Slice the monolithic raw file into the parallelized chunks expected by
  // callers.
  std::ifstream monolithic_in(monolithic_packed_path, std::ios::binary);
  if (!monolithic_in.is_open()) {
    throw std::runtime_error("Can't open unpacked monolithic sequence file.");
  }

  if (std::cmp_greater(encoding_thread_count,
                       compression_params::ReadMetadata::kFileLenThrSize)) {
    throw std::runtime_error(
        std::string("Archive indicates too many sequence chunks "
                    "(encoding_thread_count=") +
        std::to_string(encoding_thread_count) + ") for metadata array size (" +
        std::to_string(compression_params::ReadMetadata::kFileLenThrSize) +
        "). Increase array size in params.h or recreate archive.");
  }

  for (int tid = 0; tid < encoding_thread_count; tid++) {
    const std::string chunk_path =
        packed_seq_base_path + '.' + std::to_string(tid);
    std::ofstream chunk_out(chunk_path, std::ios::binary);

    uint64_t chunk_bytes;
    if (!monolithic_in.read(reinterpret_cast<char *>(&chunk_bytes),
                            sizeof(uint64_t))) {
      throw std::runtime_error(
          "Corrupt archive: failed reading packed chunk size header for tid=" +
          std::to_string(tid));
    }
    SPRING_LOG_DEBUG("Slicing chunk tid=" + std::to_string(tid) +
                     ", packed_size=" + std::to_string(chunk_bytes));

    std::vector<char> buffer(1U << 20); // 1MB buffer
    uint64_t bytes_remaining = chunk_bytes;

    while (bytes_remaining > 0) {
      uint64_t bytes_to_read =
          std::min(bytes_remaining, static_cast<uint64_t>(buffer.size()));
      monolithic_in.read(buffer.data(),
                         static_cast<std::streamsize>(bytes_to_read));
      const uint64_t bytes_read = static_cast<uint64_t>(monolithic_in.gcount());
      if (bytes_read != bytes_to_read) {
        throw std::runtime_error(
            "Corrupt archive: truncated packed sequence stream while "
            "slicing monolithic data");
      }
      chunk_out.write(buffer.data(),
                      static_cast<std::streamsize>(bytes_to_read));
      bytes_remaining -= bytes_to_read;
    }
    chunk_out.close();
  }
  monolithic_in.close();
  safe_remove_file(monolithic_packed_path);
  SPRING_LOG_DEBUG(
      "decompress_unpack_seq slicing complete; starting per-chunk decode.");

  std::exception_ptr decode_exception;
#pragma omp parallel
  {
    const int thread_id = omp_get_thread_num();
    const thread_range range = split_thread_range(
        encoding_thread_count, thread_id, decoding_thread_count);
    for (uint64_t encoding_thread_id = range.begin;
         encoding_thread_id < range.end; encoding_thread_id++) {
      try {
        decode_packed_sequence_chunk(
            packed_seq_base_path, encoding_thread_id,
            cp.read_info.file_len_seq_thr[encoding_thread_id],
            cp.encoding.methyl_ternary);
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
