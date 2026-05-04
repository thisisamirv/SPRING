// Writes reordered alignment-related streams, unaligned payloads, and per-block
// compressed outputs that become part of the final Spring archive.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <omp.h>
#include <string>
#include <vector>

#include "fs_utils.h"
#include "libbsc/libbsc.h"
#include "params.h"
#include "progress.h"
#include "reordered_streams.h"

namespace spring {

namespace {

#pragma pack(push, 1)
struct bsc_block_header {
  long long block_offset;
  signed char record_size;
  signed char sorting_contexts;
};
#pragma pack(pop)

struct reordered_stream_paths {
  std::string flag_path;
  std::string position_path;
  std::string mate_position_path;
  std::string orientation_path;
  std::string mate_orientation_path;
  std::string read_length_path;
  std::string unaligned_path;
  std::string noise_path;
  std::string noise_position_path;
  std::string order_path;
};

struct output_block_buffers {
  std::vector<char> flag_bytes;
  std::vector<char> position_bytes;
  std::vector<char> noise_bytes;
  std::vector<char> noise_position_bytes;
  std::vector<char> orientation_bytes;
  std::vector<char> unaligned_bytes;
  std::vector<char> read_length_bytes;
  std::vector<char> mate_position_bytes;
  std::vector<char> mate_orientation_bytes;
};

struct block_range {
  uint64_t begin;
  uint64_t end;
  bool valid;
};

std::string block_file_path(const std::string &base_path,
                            const uint64_t block_num) {
  return base_path + '.' + std::to_string(block_num);
}

std::string compressed_block_file_path(const std::string &base_path,
                                       const uint64_t block_num) {
  return block_file_path(base_path, block_num) + ".bsc";
}

void ensure_libbsc_ready() {
  static std::once_flag init_once;
  std::call_once(init_once, []() {
    const int init_result = ::bsc_init(LIBBSC_DEFAULT_FEATURES);
    if (init_result != LIBBSC_NO_ERROR) {
      throw std::runtime_error("Failed to initialize libbsc.");
    }
  });
}

void compress_block_buffer(const std::vector<char> &input_bytes,
                           const std::string &output_path) {
  if (input_bytes.empty()) {
    std::ofstream empty_output(output_path, std::ios::binary | std::ios::trunc);
    if (!empty_output.is_open()) {
      throw std::runtime_error("Failed to create empty compressed block: " +
                               output_path);
    }
    return;
  }

  ensure_libbsc_ready();
  if (input_bytes.size() >
      static_cast<size_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("Block too large for libbsc compressor: " +
                             output_path);
  }

  std::vector<unsigned char> compressed(input_bytes.size() +
                                        LIBBSC_HEADER_SIZE);
  int compressed_size = ::bsc_compress(
      reinterpret_cast<const unsigned char *>(input_bytes.data()),
      compressed.data(), static_cast<int>(input_bytes.size()), 16, 128,
      LIBBSC_BLOCKSORTER_BWT, LIBBSC_CODER_QLFC_STATIC,
      LIBBSC_DEFAULT_FEATURES);
  if (compressed_size == LIBBSC_NOT_COMPRESSIBLE) {
    compressed_size =
        ::bsc_store(reinterpret_cast<const unsigned char *>(input_bytes.data()),
                    compressed.data(), static_cast<int>(input_bytes.size()),
                    LIBBSC_DEFAULT_FEATURES);
  }
  if (compressed_size < LIBBSC_NO_ERROR) {
    throw std::runtime_error("libbsc compression failed for block: " +
                             output_path);
  }

  std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
  if (!output.is_open()) {
    throw std::runtime_error("Failed to open compressed block output: " +
                             output_path);
  }

  const int nblocks = 1;
  const bsc_block_header header = {
      0,
      1,
      static_cast<signed char>(1) /* LIBBSC_CONTEXTS_FOLLOWING */,
  };

  output.write(reinterpret_cast<const char *>(&nblocks),
               static_cast<std::streamsize>(sizeof(nblocks)));
  output.write(reinterpret_cast<const char *>(&header),
               static_cast<std::streamsize>(sizeof(header)));
  output.write(reinterpret_cast<const char *>(compressed.data()),
               static_cast<std::streamsize>(compressed_size));
}

reordered_stream_paths
build_reordered_stream_paths(const std::string &temp_dir) {
  return {.flag_path = temp_dir + "/read_flag.txt",
          .position_path = temp_dir + "/read_pos.bin",
          .mate_position_path = temp_dir + "/read_pos_pair.bin",
          .orientation_path = temp_dir + "/read_rev.txt",
          .mate_orientation_path = temp_dir + "/read_rev_pair.txt",
          .read_length_path = temp_dir + "/read_lengths.bin",
          .unaligned_path = temp_dir + "/read_unaligned.txt",
          .noise_path = temp_dir + "/read_noise.txt",
          .noise_position_path = temp_dir + "/read_noisepos.bin",
          .order_path = temp_dir + "/read_order.bin"};
}

block_range block_read_range(const uint64_t block_num,
                             const uint32_t num_reads_per_block,
                             const uint64_t read_limit) {
  const uint64_t begin = block_num * num_reads_per_block;
  if (begin >= read_limit) {
    return {.begin = 0, .end = 0, .valid = false};
  }

  const uint64_t end =
      std::min((block_num + 1) * uint64_t(num_reads_per_block), read_limit);
  return {.begin = begin, .end = end, .valid = true};
}

template <typename T>
void append_binary(std::vector<char> &buffer, const T &value) {
  const size_t old_size = buffer.size();
  buffer.resize(old_size + sizeof(T));
  std::memcpy(buffer.data() + old_size, &value, sizeof(T));
}

void decode_unaligned_reads(const std::vector<char> &encoded,
                            const uint32_t expected_read_count,
                            std::vector<char> &decoded_chars,
                            std::vector<uint16_t> &decoded_lengths,
                            bool bisulfite_ternary) {
  (void)bisulfite_ternary;
  decoded_lengths.assign(expected_read_count, 0);
  std::vector<uint64_t> encoded_offsets(expected_read_count, 0);
  std::vector<uint64_t> decoded_offsets(expected_read_count, 0);

  uint64_t encoded_cursor = 0;
  uint64_t decoded_total = 0;
  for (uint32_t read_index = 0; read_index < expected_read_count;
       ++read_index) {
    if (encoded_cursor + sizeof(uint32_t) > encoded.size()) {
      throw std::runtime_error(
          "Corrupted unaligned stream: truncated read length header.");
    }

    uint32_t read_length = 0;
    std::memcpy(&read_length, encoded.data() + encoded_cursor,
                sizeof(uint32_t));
    encoded_cursor += sizeof(uint32_t);

    const uint64_t encoded_bytes = static_cast<uint64_t>(read_length);
    if (encoded_cursor + encoded_bytes > encoded.size()) {
      throw std::runtime_error(
          "Corrupted unaligned stream: truncated raw payload.");
    }

    decoded_lengths[read_index] = static_cast<uint16_t>(read_length);
    encoded_offsets[read_index] = encoded_cursor;
    decoded_offsets[read_index] = decoded_total;
    decoded_total += read_length;
    encoded_cursor += encoded_bytes;
  }

  if (encoded_cursor != encoded.size()) {
    throw std::runtime_error(
        "Corrupted unaligned stream: trailing bytes after expected records.");
  }

  decoded_chars.assign(decoded_total, 0);

#pragma omp parallel for schedule(static)
  for (size_t read_index = 0; read_index < expected_read_count; ++read_index) {
    const uint16_t read_length = decoded_lengths[read_index];
    const char *encoded_read = encoded.data() + encoded_offsets[read_index];
    char *decoded_read = decoded_chars.data() + decoded_offsets[read_index];
    std::memcpy(decoded_read, encoded_read, read_length);
  }
}

void write_noise_for_read(std::vector<char> &noise_output,
                          std::vector<char> &noise_position_output,
                          const std::vector<char> &noise_codes,
                          const std::vector<uint16_t> &noise_positions,
                          const std::vector<uint64_t> &noise_offset_by_read,
                          const std::vector<uint16_t> &noise_count_by_read,
                          const uint64_t read_index) {
  for (uint16_t noise_index = 0; noise_index < noise_count_by_read[read_index];
       noise_index++) {
    noise_output.push_back(
        noise_codes[noise_offset_by_read[read_index] + noise_index]);
    append_binary(
        noise_position_output,
        noise_positions[noise_offset_by_read[read_index] + noise_index]);
  }
  noise_output.push_back('\n');
}

void write_unaligned_read(std::vector<char> &unaligned_output,
                          const std::vector<char> &unaligned_chars,
                          const std::vector<uint64_t> &position_by_read,
                          const std::vector<uint16_t> &read_lengths_by_read,
                          const uint64_t read_index) {
  const uint64_t offset = position_by_read[read_index];
  const uint16_t read_length = read_lengths_by_read[read_index];
  unaligned_output.insert(unaligned_output.end(),
                          unaligned_chars.begin() + offset,
                          unaligned_chars.begin() + offset + read_length);
}

void write_aligned_position(std::vector<char> &position_output,
                            const bool preserve_order,
                            const bool first_read_in_block,
                            const uint64_t current_position,
                            uint64_t &previous_position) {
  if (preserve_order || first_read_in_block) {
    append_binary(position_output, current_position);
    previous_position = current_position;
    return;
  }

  const uint64_t position_delta = current_position - previous_position;
  uint16_t encoded_delta = 0;
  if (position_delta < 65535) {
    encoded_delta = static_cast<uint16_t>(position_delta);
    append_binary(position_output, encoded_delta);
  } else {
    encoded_delta = 65535;
    append_binary(position_output, encoded_delta);
    append_binary(position_output, current_position);
  }
  previous_position = current_position;
}

void cleanup_consumed_input_files(const reordered_stream_paths &paths) {
  safe_remove_file(paths.order_path);
}

void compress_output_block(const output_block_buffers &block_buffers,
                           const reordered_stream_paths &paths,
                           const uint64_t block_num, const bool paired_end) {
  compress_block_buffer(block_buffers.flag_bytes,
                        compressed_block_file_path(paths.flag_path, block_num));
  compress_block_buffer(
      block_buffers.position_bytes,
      compressed_block_file_path(paths.position_path, block_num));
  compress_block_buffer(
      block_buffers.noise_bytes,
      compressed_block_file_path(paths.noise_path, block_num));
  compress_block_buffer(
      block_buffers.noise_position_bytes,
      compressed_block_file_path(paths.noise_position_path, block_num));
  compress_block_buffer(
      block_buffers.unaligned_bytes,
      compressed_block_file_path(paths.unaligned_path, block_num));
  compress_block_buffer(
      block_buffers.read_length_bytes,
      compressed_block_file_path(paths.read_length_path, block_num));
  compress_block_buffer(
      block_buffers.orientation_bytes,
      compressed_block_file_path(paths.orientation_path, block_num));

  if (!paired_end) {
    return;
  }

  compress_block_buffer(
      block_buffers.mate_position_bytes,
      compressed_block_file_path(paths.mate_position_path, block_num));
  compress_block_buffer(
      block_buffers.mate_orientation_bytes,
      compressed_block_file_path(paths.mate_orientation_path, block_num));
}

} // namespace

void reorder_compress_streams(
    const std::string &temp_dir, const compression_params &cp,
    const reordered_stream_artifact &artifact,
    const std::vector<uint32_t> *read_order_override) {
  const reordered_stream_paths paths = build_reordered_stream_paths(temp_dir);
  SPRING_LOG_DEBUG(
      "reorder_compress_streams start: temp_dir=" + temp_dir +
      ", num_reads=" + std::to_string(cp.read_info.num_reads) +
      ", paired_end=" + std::string(cp.encoding.paired_end ? "true" : "false") +
      ", preserve_order=" +
      std::string(cp.encoding.preserve_order ? "true" : "false") +
      ", threads=" + std::to_string(cp.encoding.num_thr));

  const uint32_t num_reads = cp.read_info.num_reads;
  uint32_t aligned_read_count = 0;
  uint32_t unaligned_read_count = 0;
  const uint32_t half_read_count = num_reads / 2;
  const int num_thr = cp.encoding.num_thr;
  const bool paired_end = cp.encoding.paired_end;
  const bool preserve_order = cp.encoding.preserve_order;

  std::vector<char> orientation_by_read(num_reads);
  std::vector<uint16_t> read_lengths_by_read(num_reads);
  std::vector<bool> aligned_flags(num_reads);
  std::vector<uint64_t> noise_offset_by_read(num_reads);
  std::vector<uint64_t> position_by_read(num_reads);
  std::vector<uint16_t> noise_count_by_read(num_reads);

  const std::vector<char> &orientation_entries = artifact.orientation_entries;
  const std::vector<uint64_t> &position_entries = artifact.position_entries;
  const std::vector<uint16_t> &read_length_entries =
      artifact.read_length_entries;
  const std::vector<uint32_t> *read_order_entries =
      read_order_override != nullptr ? read_order_override
                                     : &artifact.read_order_entries;
  const std::vector<char> &noise_serialized = artifact.noise_serialized;
  const std::vector<uint16_t> &noise_positions = artifact.noise_positions;

  if (orientation_entries.size() != position_entries.size()) {
    throw std::runtime_error(
        "Corruption in aligned streams: orientation/position size mismatch.");
  }

  aligned_read_count = static_cast<uint32_t>(orientation_entries.size());
  if (aligned_read_count > num_reads) {
    throw std::runtime_error("Corruption in aligned streams: aligned read "
                             "count exceeds total reads.");
  }

  if (read_length_entries.size() != num_reads) {
    throw std::runtime_error(
        "Corruption in read length stream: entry count does not match reads.");
  }

  if ((paired_end || preserve_order) &&
      read_order_entries->size() != num_reads) {
    throw std::runtime_error(
        "Corruption in read order stream: entry count does not match reads.");
  }

  uint64_t next_noise_offset = 0;
  size_t noise_cursor = 0;
  std::vector<char> noise_codes(noise_positions.size());

  for (uint32_t entry_index = 0; entry_index < aligned_read_count;
       ++entry_index) {
    const uint32_t read_order = (paired_end || preserve_order)
                                    ? (*read_order_entries)[entry_index]
                                    : entry_index;

    orientation_by_read[read_order] = orientation_entries[entry_index];
    read_lengths_by_read[read_order] = read_length_entries[entry_index];
    aligned_flags[read_order] = true;
    position_by_read[read_order] = position_entries[entry_index];
    noise_offset_by_read[read_order] = next_noise_offset;

    const char *line_begin = noise_serialized.data() + noise_cursor;
    const size_t bytes_remaining = noise_serialized.size() - noise_cursor;
    const void *line_end_ptr = std::memchr(line_begin, '\n', bytes_remaining);
    const size_t line_len =
        line_end_ptr == nullptr
            ? bytes_remaining
            : static_cast<size_t>(static_cast<const char *>(line_end_ptr) -
                                  line_begin);

    if (next_noise_offset + line_len > noise_positions.size()) {
      throw std::runtime_error(
          "Corruption in noise stream: excess codes found beyond position "
          "metadata limit.");
    }

    if (line_len > 0) {
      std::memcpy(noise_codes.data() + next_noise_offset, line_begin, line_len);
    }
    noise_count_by_read[read_order] = static_cast<uint16_t>(line_len);
    next_noise_offset += line_len;
    noise_cursor += line_len;
    if (line_end_ptr != nullptr) {
      noise_cursor += 1;
    }
  }

  while (noise_cursor < noise_serialized.size() &&
         noise_serialized[noise_cursor] == '\n') {
    noise_cursor++;
  }

  if (noise_cursor != noise_serialized.size()) {
    throw std::runtime_error(
        "Corruption in noise stream: extra non-delimiter payload after "
        "aligned entries.");
  }

  if (next_noise_offset != noise_positions.size()) {
    throw std::runtime_error(
        "Corruption in noise stream: code/position entry count mismatch.");
  }

  unaligned_read_count = num_reads - aligned_read_count;
  SPRING_LOG_DEBUG(
      "reorder_compress_streams parsed input streams: aligned_reads=" +
      std::to_string(aligned_read_count) +
      ", unaligned_reads=" + std::to_string(unaligned_read_count) +
      ", noise_entries=" + std::to_string(noise_positions.size()));

  std::vector<char> unaligned_chars;
  std::vector<uint16_t> unaligned_lengths;
  decode_unaligned_reads(artifact.unaligned_serialized, unaligned_read_count,
                         unaligned_chars, unaligned_lengths,
                         cp.encoding.bisulfite_ternary);
  if (unaligned_chars.size() != artifact.unaligned_char_count) {
    throw std::runtime_error(
        "Corruption in unaligned stream: decoded size does not match recorded "
        "character count.");
  }

  uint64_t current_unaligned_offset = 0;
  for (uint32_t read_index = 0; read_index < unaligned_read_count;
       read_index++) {
    const uint32_t read_order =
        (paired_end || preserve_order)
            ? (*read_order_entries)[aligned_read_count + read_index]
            : (aligned_read_count + read_index);
    const uint16_t read_length =
        read_length_entries[aligned_read_count + read_index];
    if (unaligned_lengths[read_index] != read_length) {
      throw std::runtime_error(
          "Corruption in unaligned stream: decoded read length does not match "
          "read-length metadata.");
    }
    read_lengths_by_read[read_order] = read_length;
    position_by_read[read_order] = current_unaligned_offset;
    current_unaligned_offset += read_length;
    aligned_flags[read_order] = false;
  }

  cleanup_consumed_input_files(paths);

  omp_set_num_threads(num_thr);
  const uint32_t num_reads_per_block = cp.encoding.num_reads_per_block;
  const uint64_t read_limit = paired_end ? half_read_count : num_reads;
  const uint64_t output_blocks =
      (read_limit == 0)
          ? 0
          : (read_limit + static_cast<uint64_t>(num_reads_per_block) - 1) /
                static_cast<uint64_t>(num_reads_per_block);
  SPRING_LOG_DEBUG("reorder_compress_streams output planning: read_limit=" +
                   std::to_string(read_limit) + ", num_reads_per_block=" +
                   std::to_string(num_reads_per_block) +
                   ", output_blocks=" + std::to_string(output_blocks));

#pragma omp parallel
  {
    uint64_t thread_id = omp_get_thread_num();
    uint64_t block_num = thread_id;
    while (true) {
      const block_range current_block =
          block_read_range(block_num, num_reads_per_block, read_limit);
      if (!current_block.valid) {
        break;
      }

      output_block_buffers block_buffers;
      uint64_t previous_position = 0;
      for (uint64_t read_index = current_block.begin;
           read_index < current_block.end; read_index++) {
        if (!paired_end) {
          append_binary(block_buffers.read_length_bytes,
                        read_lengths_by_read[read_index]);
          if (aligned_flags[read_index]) {
            block_buffers.flag_bytes.push_back('0');
            block_buffers.orientation_bytes.push_back(
                orientation_by_read[read_index]);
            write_aligned_position(block_buffers.position_bytes, preserve_order,
                                   read_index == current_block.begin,
                                   position_by_read[read_index],
                                   previous_position);
            write_noise_for_read(
                block_buffers.noise_bytes, block_buffers.noise_position_bytes,
                noise_codes, noise_positions, noise_offset_by_read,
                noise_count_by_read, read_index);
          } else {
            block_buffers.flag_bytes.push_back('2');
            write_unaligned_read(block_buffers.unaligned_bytes, unaligned_chars,
                                 position_by_read, read_lengths_by_read,
                                 read_index);
          }
        } else {
          const uint64_t mate_read_index = half_read_count + read_index;
          append_binary(block_buffers.read_length_bytes,
                        read_lengths_by_read[read_index]);
          append_binary(block_buffers.read_length_bytes,
                        read_lengths_by_read[mate_read_index]);
          const int64_t mate_position_delta =
              static_cast<int64_t>(position_by_read[mate_read_index]) -
              static_cast<int64_t>(position_by_read[read_index]);
          int read_flag = 2;
          if (aligned_flags[read_index] && aligned_flags[mate_read_index] &&
              mate_position_delta >= 0 && mate_position_delta < 32767) {
            read_flag = 0;
          } else if (aligned_flags[read_index] &&
                     aligned_flags[mate_read_index]) {
            read_flag = 1;
          } else if (!aligned_flags[read_index] &&
                     !aligned_flags[mate_read_index]) {
            read_flag = 2;
          } else if (aligned_flags[read_index] &&
                     !aligned_flags[mate_read_index]) {
            read_flag = 3;
          } else if (!aligned_flags[read_index] &&
                     aligned_flags[mate_read_index]) {
            read_flag = 4;
          }

          block_buffers.flag_bytes.push_back(
              static_cast<char>('0' + read_flag));
          if (read_flag == 0) {
            const int16_t mate_position_delta_16 =
                static_cast<int16_t>(mate_position_delta);
            append_binary(block_buffers.mate_position_bytes,
                          mate_position_delta_16);
            block_buffers.mate_orientation_bytes.push_back(
                orientation_by_read[read_index] !=
                        orientation_by_read[mate_read_index]
                    ? '0'
                    : '1');
          }
          if (read_flag == 0 || read_flag == 1 || read_flag == 3) {
            write_aligned_position(block_buffers.position_bytes, preserve_order,
                                   read_index == current_block.begin,
                                   position_by_read[read_index],
                                   previous_position);
            write_noise_for_read(
                block_buffers.noise_bytes, block_buffers.noise_position_bytes,
                noise_codes, noise_positions, noise_offset_by_read,
                noise_count_by_read, read_index);
            block_buffers.orientation_bytes.push_back(
                orientation_by_read[read_index]);
          } else {
            write_unaligned_read(block_buffers.unaligned_bytes, unaligned_chars,
                                 position_by_read, read_lengths_by_read,
                                 read_index);
          }

          if (read_flag == 0 || read_flag == 1 || read_flag == 4) {
            write_noise_for_read(
                block_buffers.noise_bytes, block_buffers.noise_position_bytes,
                noise_codes, noise_positions, noise_offset_by_read,
                noise_count_by_read, mate_read_index);
            if (read_flag == 1 || read_flag == 4) {
              append_binary(block_buffers.position_bytes,
                            position_by_read[mate_read_index]);
              block_buffers.orientation_bytes.push_back(
                  orientation_by_read[mate_read_index]);
            }
          } else {
            write_unaligned_read(block_buffers.unaligned_bytes, unaligned_chars,
                                 position_by_read, read_lengths_by_read,
                                 mate_read_index);
          }
        }
      }

      compress_output_block(block_buffers, paths, block_num, paired_end);
      block_num += num_thr;
    }
  }

  SPRING_LOG_DEBUG("reorder_compress_streams complete: blocks_written=" +
                   std::to_string(output_blocks));
}

} // namespace spring