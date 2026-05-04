// Implements archive-loading helpers used by decompression separately from the
// record reconstruction loops.

#include "decompress_archive_io.h"

#include "io_utils.h"
#include "progress.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <omp.h>
#include <stdexcept>
#include <utility>

namespace spring {

namespace {

struct thread_range {
  uint64_t begin;
  uint64_t end;
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

} // namespace

std::vector<char>
archive_member_bytes(const decompression_archive_artifact &artifact,
                     const std::string &member_name) {
  const std::string &contents = artifact.require(member_name);
  return std::vector<char>(contents.begin(), contents.end());
}

std::vector<char>
decompress_archive_bsc_member(const decompression_archive_artifact &artifact,
                              const std::string &member_name,
                              const bool allow_raw_fallback) {
  std::vector<char> compressed_bytes =
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

reference_sequence_store::reference_sequence_store(
    const decompression_archive_artifact &artifact,
    const std::string &packed_seq_path, const int encoding_thread_count,
    const int decode_thread_count, const compression_params &cp) {
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

std::string reference_sequence_store::read(const uint64_t start_offset,
                                           const uint32_t read_length) const {
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

size_t reference_sequence_store::find_chunk_index(const uint64_t offset) const {
  auto it = std::ranges::upper_bound(start_offsets_, offset);
  if (it == start_offsets_.begin()) {
    throw std::runtime_error(
        "Reference offset out of range (offset=" + std::to_string(offset) +
        ", total=" + std::to_string(total_size_) + ")");
  }
  const size_t chunk_index =
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

uint32_t compute_thread_read_count(const uint32_t step_read_count,
                                   const uint32_t num_reads_per_block,
                                   const uint64_t thread_id) {
  return std::min((uint64_t)step_read_count,
                  (thread_id + 1) * (uint64_t)num_reads_per_block) -
         thread_id * (uint64_t)num_reads_per_block;
}

std::string decode_packed_sequence_chunk_bytes(
    const std::vector<char> &packed_bytes, const int encoding_thread_id,
    const uint64_t num_bases, const bool bisulfite_ternary) {
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
        for (int k = 0; k < 5 && bases_decoded < num_bases; k++) {
          uint8_t val = byte % 3;
          byte /= 3;
          static constexpr char kBases[3] = {'A', 'G', 'T'};
          decoded.push_back(kBases[val]);
          bases_decoded++;
        }
      } else {
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

std::vector<std::string> decompress_unpack_seq_chunks(
    const decompression_archive_artifact &artifact,
    const std::string &packed_seq_base_path, const int encoding_thread_count,
    const int decoding_thread_count, const compression_params &cp) {
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

  std::vector<char> monolithic_packed_bytes =
      decompress_archive_bsc_member(artifact, monolithic_compressed_path);
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