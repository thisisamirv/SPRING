// Writes reordered alignment-related streams, unaligned payloads, and per-block
// compressed outputs that become part of the final Spring archive.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <array>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <omp.h>
#include <string>
#include <vector>

#include "core_utils.h"
#include "dna_utils.h"
#include "fs_utils.h"
#include "libbsc/bsc.h"
#include "progress.h"
#include "reordered_streams.h"

namespace spring {

namespace {

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

struct temporary_stream_paths {
  std::string position_path;
  std::string noise_path;
  std::string noise_position_path;
  std::string orientation_path;
  std::string flag_path;
  std::string unaligned_path;
  std::string read_length_path;
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

void compress_block_file(const std::string &input_path,
                         const std::string &output_path) {
  if (is_unaligned_block_path(output_path)) {
    copy_binary_file(input_path, output_path);
    safe_remove_file(input_path);
    return;
  }

  std::error_code ec;
  const auto input_size = std::filesystem::file_size(input_path, ec);
  if (!ec && input_size == 0) {
    std::ofstream empty_output(output_path, std::ios::binary);
    if (!empty_output.is_open()) {
      throw std::runtime_error("Failed to create empty compressed block: " +
                               output_path);
    }
  } else {
    bsc::BSC_compress(input_path.c_str(), output_path.c_str());
  }
  safe_remove_file(input_path);
}

void compress_temp_block(const std::string &temp_base_path,
                         const std::string &output_base_path,
                         const uint64_t block_num) {
  compress_block_file(block_file_path(temp_base_path, block_num),
                      compressed_block_file_path(output_base_path, block_num));
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

temporary_stream_paths
build_temporary_stream_paths(const std::string &temp_dir) {
  return {.position_path = temp_dir + "/a",
          .noise_path = temp_dir + "/b",
          .noise_position_path = temp_dir + "/c",
          .orientation_path = temp_dir + "/d",
          .flag_path = temp_dir + "/e",
          .unaligned_path = temp_dir + "/f",
          .read_length_path = temp_dir + "/g"};
}

block_range block_read_range(const uint64_t block_num,
                             const uint32_t num_reads_per_block,
                             const uint64_t read_limit) {
  const uint64_t begin = block_num * num_reads_per_block;
  if (begin >= read_limit)
    return {.begin = 0, .end = 0, .valid = false};

  const uint64_t end =
      std::min((block_num + 1) * uint64_t(num_reads_per_block), read_limit);
  return {.begin = begin, .end = end, .valid = true};
}

std::vector<char> read_binary_file_all(const std::string &path) {
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input.is_open()) {
    throw std::runtime_error("Failed to open binary file: " + path);
  }
  const std::streamsize file_size = input.tellg();
  if (file_size < 0) {
    throw std::runtime_error("Failed to determine file size: " + path);
  }
  std::vector<char> data(static_cast<size_t>(file_size));
  input.seekg(0, std::ios::beg);
  if (file_size > 0 && !input.read(data.data(), file_size)) {
    throw std::runtime_error("Failed to read binary file: " + path);
  }
  return data;
}

template <typename T>
std::vector<T> read_binary_records_all(const std::string &path) {
  const std::vector<char> raw = read_binary_file_all(path);
  if (raw.size() % sizeof(T) != 0) {
    throw std::runtime_error("Corrupted record stream (size mismatch): " +
                             path);
  }
  std::vector<T> records(raw.size() / sizeof(T));
  if (!raw.empty()) {
    std::memcpy(records.data(), raw.data(), raw.size());
  }
  return records;
}

void decode_unaligned_reads(const std::string &path,
                            const uint32_t expected_read_count,
                            std::vector<char> &decoded_chars,
                            std::vector<uint16_t> &decoded_lengths) {
  const std::vector<char> encoded = read_binary_file_all(path);
  decoded_lengths.assign(expected_read_count, 0);
  std::vector<uint64_t> encoded_offsets(expected_read_count, 0);
  std::vector<uint64_t> decoded_offsets(expected_read_count, 0);

  uint64_t encoded_cursor = 0;
  uint64_t decoded_total = 0;
  for (uint32_t read_index = 0; read_index < expected_read_count; ++read_index) {
    if (encoded_cursor + sizeof(uint16_t) > encoded.size()) {
      throw std::runtime_error(
          "Corrupted unaligned stream: truncated read length header.");
    }

    uint16_t read_length;
    std::memcpy(&read_length, encoded.data() + encoded_cursor,
                sizeof(uint16_t));
    encoded_cursor += sizeof(uint16_t);

    const uint64_t encoded_bytes =
        (static_cast<uint64_t>(read_length) + 1) / 2;
    if (encoded_cursor + encoded_bytes > encoded.size()) {
      throw std::runtime_error(
          "Corrupted unaligned stream: truncated encoded payload.");
    }

    decoded_lengths[read_index] = read_length;
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
  static const std::array<char, 16> int_to_base = {
      'A', 'G', 'C', 'T', 'N', 'N', 'N', 'N',
      'N', 'N', 'N', 'N', 'N', 'N', 'N', 'N'};

#pragma omp parallel for schedule(static)
  for (int read_index = 0; read_index < static_cast<int>(expected_read_count);
       ++read_index) {
    const uint16_t read_length = decoded_lengths[read_index];
    const uint8_t *encoded_read = reinterpret_cast<const uint8_t *>(
        encoded.data() + encoded_offsets[read_index]);
    char *decoded_read = decoded_chars.data() + decoded_offsets[read_index];

    for (uint16_t base_index = 0; base_index < read_length; ++base_index) {
      const uint8_t packed_byte = encoded_read[base_index / 2];
      const uint8_t base_code =
          (base_index % 2 == 0) ? (packed_byte & 0x0F) : (packed_byte >> 4);
      decoded_read[base_index] = int_to_base[base_code & 0x0F];
    }
  }
}

void write_noise_for_read(std::ofstream &noise_output,
                          std::ofstream &noise_position_output,
                          const std::vector<char> &noise_codes,
                          const std::vector<uint16_t> &noise_positions,
                          const std::vector<uint64_t> &noise_offset_by_read,
                          const std::vector<uint16_t> &noise_count_by_read,
                          const uint64_t read_index) {
  for (uint16_t noise_index = 0; noise_index < noise_count_by_read[read_index];
       noise_index++) {
    noise_output << noise_codes[noise_offset_by_read[read_index] + noise_index];
    noise_position_output.write(
        byte_ptr(
            &noise_positions[noise_offset_by_read[read_index] + noise_index]),
        sizeof(uint16_t));
  }
  noise_output << "\n";
}

void write_unaligned_read(std::ofstream &unaligned_output,
                          const std::vector<char> &unaligned_chars,
                          const std::vector<uint64_t> &position_by_read,
                          const std::vector<uint16_t> &read_lengths_by_read,
                          const uint64_t read_index) {
  unaligned_output.write(unaligned_chars.data() + position_by_read[read_index],
                         read_lengths_by_read[read_index]);
}

void write_aligned_position(std::ofstream &position_output,
                            const bool preserve_order,
                            const bool first_read_in_block,
                            const uint64_t current_position,
                            uint64_t &previous_position) {
  if (preserve_order || first_read_in_block) {
    position_output.write(byte_ptr(&current_position), sizeof(uint64_t));
    previous_position = current_position;
    return;
  }

  const uint64_t position_delta = current_position - previous_position;
  uint16_t encoded_delta;
  if (position_delta < 65535) {
    encoded_delta = static_cast<uint16_t>(position_delta);
    position_output.write(byte_ptr(&encoded_delta), sizeof(uint16_t));
  } else {
    encoded_delta = 65535;
    position_output.write(byte_ptr(&encoded_delta), sizeof(uint16_t));
    position_output.write(byte_ptr(&current_position), sizeof(uint64_t));
  }
  previous_position = current_position;
}

void remove_input_stream_files(const reordered_stream_paths &paths) {
  safe_remove_file(paths.noise_path);
  safe_remove_file(paths.noise_position_path);
  safe_remove_file(paths.orientation_path);
  safe_remove_file(paths.order_path);
  safe_remove_file(paths.read_length_path);
  safe_remove_file(paths.unaligned_path);
  safe_remove_file(paths.position_path);
}

void compress_output_block(const temporary_stream_paths &temp_paths,
                           const reordered_stream_paths &paths,
                           const uint64_t block_num, const bool paired_end) {
  compress_temp_block(temp_paths.flag_path, paths.flag_path, block_num);
  compress_temp_block(temp_paths.position_path, paths.position_path, block_num);
  compress_temp_block(temp_paths.noise_path, paths.noise_path, block_num);
  compress_temp_block(temp_paths.noise_position_path, paths.noise_position_path,
                      block_num);
  compress_temp_block(temp_paths.unaligned_path, paths.unaligned_path,
                      block_num);
  compress_temp_block(temp_paths.read_length_path, paths.read_length_path,
                      block_num);
  compress_temp_block(temp_paths.orientation_path, paths.orientation_path,
                      block_num);

  if (!paired_end)
    return;

  compress_block_file(
      block_file_path(paths.mate_position_path, block_num),
      compressed_block_file_path(paths.mate_position_path, block_num));
  compress_block_file(
      block_file_path(paths.mate_orientation_path, block_num),
      compressed_block_file_path(paths.mate_orientation_path, block_num));
}

} // namespace

void reorder_compress_streams(const std::string &temp_dir,
                              const compression_params &cp) {
  const reordered_stream_paths paths = build_reordered_stream_paths(temp_dir);
  Logger::log_debug("reorder_compress_streams start: temp_dir=" + temp_dir +
                    ", num_reads=" + std::to_string(cp.read_info.num_reads) +
                    ", paired_end=" +
                    std::string(cp.encoding.paired_end ? "true" : "false") +
                    ", preserve_order=" +
                    std::string(cp.encoding.preserve_order ? "true" : "false") +
                    ", threads=" + std::to_string(cp.encoding.num_thr));

  uint32_t num_reads = cp.read_info.num_reads;
  uint32_t aligned_read_count = 0;
  uint32_t unaligned_read_count;
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

  std::vector<char> orientation_entries;
  std::vector<uint64_t> position_entries;
  std::vector<uint16_t> read_length_entries;
  std::vector<uint32_t> read_order_entries;
  std::vector<char> noise_serialized;
  std::vector<uint16_t> noise_positions;

#pragma omp parallel sections
  {
#pragma omp section
    { orientation_entries = read_binary_file_all(paths.orientation_path); }
#pragma omp section
    { position_entries = read_binary_records_all<uint64_t>(paths.position_path); }
#pragma omp section
    { read_length_entries = read_binary_records_all<uint16_t>(paths.read_length_path); }
#pragma omp section
    { noise_serialized = read_binary_file_all(paths.noise_path); }
#pragma omp section
    { noise_positions = read_binary_records_all<uint16_t>(paths.noise_position_path); }
#pragma omp section
    {
      if (paired_end || preserve_order) {
        read_order_entries = read_binary_records_all<uint32_t>(paths.order_path);
      }
    }
  }

  if (orientation_entries.size() != position_entries.size()) {
    throw std::runtime_error(
        "Corruption in aligned streams: orientation/position size mismatch.");
  }

  aligned_read_count = static_cast<uint32_t>(orientation_entries.size());
  if (aligned_read_count > num_reads) {
    throw std::runtime_error(
        "Corruption in aligned streams: aligned read count exceeds total reads.");
  }

  if (read_length_entries.size() != num_reads) {
    throw std::runtime_error(
        "Corruption in read length stream: entry count does not match reads.");
  }

  if ((paired_end || preserve_order) && read_order_entries.size() != num_reads) {
    throw std::runtime_error(
        "Corruption in read order stream: entry count does not match reads.");
  }

  uint64_t next_noise_offset = 0;
  size_t noise_cursor = 0;
  std::vector<char> noise_codes(noise_positions.size());

  for (uint32_t entry_index = 0; entry_index < aligned_read_count;
       ++entry_index) {
    const uint32_t read_order =
        (paired_end || preserve_order) ? read_order_entries[entry_index]
                                       : entry_index;

    orientation_by_read[read_order] = orientation_entries[entry_index];
    read_lengths_by_read[read_order] = read_length_entries[entry_index];
    aligned_flags[read_order] = true;
    position_by_read[read_order] = position_entries[entry_index];
    noise_offset_by_read[read_order] = next_noise_offset;

    const char *line_begin = noise_serialized.data() + noise_cursor;
    const size_t bytes_remaining = noise_serialized.size() - noise_cursor;
    const void *line_end_ptr = std::memchr(line_begin, '\n', bytes_remaining);
    const size_t line_len = line_end_ptr == nullptr
                                ? bytes_remaining
                                : static_cast<size_t>(
                                      static_cast<const char *>(line_end_ptr) -
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
  Logger::log_debug("reorder_compress_streams parsed input streams: aligned_reads=" +
                    std::to_string(aligned_read_count) +
                    ", unaligned_reads=" + std::to_string(unaligned_read_count) +
                    ", noise_entries=" +
                    std::to_string(noise_positions.size()));
  std::string unaligned_count_path = paths.unaligned_path + ".count";
  std::ifstream unaligned_count_input(unaligned_count_path, std::ios::binary);
  uint64_t unaligned_char_count;
  unaligned_count_input.read(byte_ptr(&unaligned_char_count), sizeof(uint64_t));
  unaligned_count_input.close();
  remove(unaligned_count_path.c_str());
  std::vector<char> unaligned_chars;
  std::vector<uint16_t> unaligned_lengths;
  decode_unaligned_reads(paths.unaligned_path, unaligned_read_count,
                         unaligned_chars, unaligned_lengths);
  if (unaligned_chars.size() != unaligned_char_count) {
    throw std::runtime_error(
        "Corruption in unaligned stream: decoded size does not match recorded "
        "character count.");
  }

  // Both aligned and unaligned lengths live sequentially in read_length_input
  // (write_unaligned_range appends to the same file). Read them all in order.
  uint64_t current_unaligned_offset = 0;
  for (uint32_t read_index = 0; read_index < unaligned_read_count;
       read_index++) {
    const uint32_t read_order = (paired_end || preserve_order)
                                    ? read_order_entries[aligned_read_count + read_index]
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

  remove_input_stream_files(paths);

  omp_set_num_threads(num_thr);
  const uint32_t num_reads_per_block = cp.encoding.num_reads_per_block;
  const temporary_stream_paths temp_paths =
      build_temporary_stream_paths(temp_dir);
  const uint64_t read_limit = paired_end ? half_read_count : num_reads;
    const uint64_t output_blocks =
      (read_limit == 0)
        ? 0
        : (read_limit + static_cast<uint64_t>(num_reads_per_block) - 1) /
          static_cast<uint64_t>(num_reads_per_block);
    Logger::log_debug("reorder_compress_streams output planning: read_limit=" +
            std::to_string(read_limit) +
            ", num_reads_per_block=" +
            std::to_string(num_reads_per_block) +
            ", output_blocks=" + std::to_string(output_blocks));

  // In paired-end mode, the block indices count read pairs rather than reads.
#pragma omp parallel
  {
    uint64_t thread_id = omp_get_thread_num();
    uint64_t block_num = thread_id;
    while (true) {
      const block_range current_block =
          block_read_range(block_num, num_reads_per_block, read_limit);
      if (!current_block.valid)
        break;

      std::ofstream flag_output(
          block_file_path(temp_paths.flag_path, block_num), std::ios::binary);
      std::ofstream noise_output(
          block_file_path(temp_paths.noise_path, block_num), std::ios::binary);
      std::ofstream noise_position_output(
          block_file_path(temp_paths.noise_position_path, block_num),
          std::ios::binary);
      std::ofstream position_output(
          block_file_path(temp_paths.position_path, block_num),
          std::ios::binary);
      std::ofstream orientation_output(
          block_file_path(temp_paths.orientation_path, block_num),
          std::ios::binary);
      std::ofstream unaligned_output(
          block_file_path(temp_paths.unaligned_path, block_num),
          std::ios::binary);
      std::ofstream read_length_output(
          block_file_path(temp_paths.read_length_path, block_num),
          std::ios::binary);
      std::ofstream mate_position_output;
      std::ofstream mate_orientation_output;
      if (paired_end) {
        mate_position_output.open(
            block_file_path(paths.mate_position_path, block_num),
            std::ios::binary);
        mate_orientation_output.open(
            block_file_path(paths.mate_orientation_path, block_num),
            std::ios::binary);
      }

      uint64_t previous_position = 0;
      for (uint64_t read_index = current_block.begin;
           read_index < current_block.end; read_index++) {
        if (!paired_end) {
          read_length_output.write(byte_ptr(&read_lengths_by_read[read_index]),
                                   sizeof(uint16_t));
          if (aligned_flags[read_index] == true) {
            flag_output << '0';
            orientation_output << orientation_by_read[read_index];
            write_aligned_position(position_output, preserve_order,
                                   read_index == current_block.begin,
                                   position_by_read[read_index],
                                   previous_position);
            write_noise_for_read(noise_output, noise_position_output,
                                 noise_codes, noise_positions,
                                 noise_offset_by_read, noise_count_by_read,
                                 read_index);
          } else {
            flag_output << '2';
            write_unaligned_read(unaligned_output, unaligned_chars,
                                 position_by_read, read_lengths_by_read,
                                 read_index);
          }
        } else {
          uint64_t mate_read_index = half_read_count + read_index;
          read_length_output.write(byte_ptr(&read_lengths_by_read[read_index]),
                                   sizeof(uint16_t));
          read_length_output.write(
              byte_ptr(&read_lengths_by_read[mate_read_index]),
              sizeof(uint16_t));
          int64_t mate_position_delta =
              (int64_t)position_by_read[mate_read_index] -
              (int64_t)position_by_read[read_index];
          int read_flag = 2;
          if (aligned_flags[read_index] && aligned_flags[mate_read_index] &&
              mate_position_delta >= 0 && mate_position_delta < 32767)
            read_flag = 0;
          else if (aligned_flags[read_index] && aligned_flags[mate_read_index])
            read_flag = 1;
          else if (!aligned_flags[read_index] &&
                   !aligned_flags[mate_read_index])
            read_flag = 2;
          else if (aligned_flags[read_index] && !aligned_flags[mate_read_index])
            read_flag = 3;
          else if (!aligned_flags[read_index] && aligned_flags[mate_read_index])
            read_flag = 4;
          flag_output << read_flag;
          if (read_flag == 0 && paired_end) {
            int16_t mate_position_delta_16 = (int16_t)mate_position_delta;
            mate_position_output.write(byte_ptr(&mate_position_delta_16),
                                       sizeof(int16_t));
            if (orientation_by_read[read_index] !=
                orientation_by_read[mate_read_index])
              mate_orientation_output << '0';
            else
              mate_orientation_output << '1';
          }
          if (read_flag == 0 || read_flag == 1 || read_flag == 3) {
            // read 1 is aligned
            write_aligned_position(position_output, preserve_order,
                                   read_index == current_block.begin,
                                   position_by_read[read_index],
                                   previous_position);
            write_noise_for_read(noise_output, noise_position_output,
                                 noise_codes, noise_positions,
                                 noise_offset_by_read, noise_count_by_read,
                                 read_index);
            orientation_output << orientation_by_read[read_index];
          } else {
            write_unaligned_read(unaligned_output, unaligned_chars,
                                 position_by_read, read_lengths_by_read,
                                 read_index);
          }

          if (read_flag == 0 || read_flag == 1 || read_flag == 4) {
            write_noise_for_read(noise_output, noise_position_output,
                                 noise_codes, noise_positions,
                                 noise_offset_by_read, noise_count_by_read,
                                 mate_read_index);
            if (read_flag == 1 || read_flag == 4) {
              position_output.write(
                  byte_ptr(&position_by_read[mate_read_index]),
                  sizeof(uint64_t));
              orientation_output << orientation_by_read[mate_read_index];
            }
          } else {
            write_unaligned_read(unaligned_output, unaligned_chars,
                                 position_by_read, read_lengths_by_read,
                                 mate_read_index);
          }
        }
      }

      flag_output.close();
      noise_output.close();
      noise_position_output.close();
      position_output.close();
      orientation_output.close();
      unaligned_output.close();
      read_length_output.close();
      if (paired_end) {
        mate_position_output.close();
        mate_orientation_output.close();
      }

      compress_output_block(temp_paths, paths, block_num, paired_end);

      block_num += num_thr;
    }
  }

  Logger::log_debug("reorder_compress_streams complete: blocks_written=" +
                    std::to_string(output_blocks));

  return;
}

} // namespace spring
