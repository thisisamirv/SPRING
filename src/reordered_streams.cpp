// Writes reordered alignment-related streams, unaligned payloads, and per-block
// compressed outputs that become part of the final Spring archive.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <omp.h>
#include <string>
#include <vector>

#include "libbsc/bsc.h"
#include "reordered_streams.h"
#include "util.h"

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

void compress_block_file(const std::string &input_path,
                         const std::string &output_path) {
  bsc::BSC_compress(input_path.c_str(), output_path.c_str());
  remove(input_path.c_str());
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
  remove(paths.noise_path.c_str());
  remove(paths.noise_position_path.c_str());
  remove(paths.orientation_path.c_str());
  remove(paths.order_path.c_str());
  remove(paths.read_length_path.c_str());
  remove(paths.unaligned_path.c_str());
  remove(paths.position_path.c_str());
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

  uint32_t num_reads = cp.num_reads;
  uint32_t aligned_read_count = 0;
  uint32_t unaligned_read_count;
  const uint32_t half_read_count = num_reads / 2;
  const int num_thr = cp.num_thr;
  const bool paired_end = cp.paired_end;
  const bool preserve_order = cp.preserve_order;

  std::vector<char> orientation_by_read(num_reads);
  std::vector<uint16_t> read_lengths_by_read(num_reads);
  std::vector<bool> aligned_flags(num_reads);
  std::vector<uint64_t> noise_offset_by_read(num_reads);
  std::vector<uint64_t> position_by_read(num_reads);
  std::vector<uint16_t> noise_count_by_read(num_reads);

  std::ifstream order_input;
  if (paired_end || preserve_order)
    order_input.open(paths.order_path, std::ios::binary);
  std::ifstream orientation_input(paths.orientation_path, std::ios::binary);
  std::ifstream read_length_input(paths.read_length_path, std::ios::binary);
  std::ifstream noise_input(paths.noise_path, std::ios::binary);
  std::ifstream noise_position_input(paths.noise_position_path,
                                     std::ios::binary);
  std::ifstream position_input(paths.position_path, std::ios::binary);
  noise_position_input.seekg(0, noise_position_input.end);
  uint64_t noise_entry_count = noise_position_input.tellg() / 2;
  noise_position_input.seekg(0, noise_position_input.beg);
  std::vector<char> noise_codes(noise_entry_count);
  std::vector<uint16_t> noise_positions(noise_entry_count);
  char orientation_char;
  char noise_code;
  uint32_t read_order = 0;
  uint64_t next_noise_offset = 0;
  uint64_t next_noise_position_offset = 0;
  uint64_t read_position;
  uint16_t noise_count_for_read;
  uint16_t read_length;
  uint16_t noise_position;

  while (orientation_input.get(orientation_char)) {
    if (paired_end || preserve_order)
      order_input.read(byte_ptr(&read_order), sizeof(uint32_t));
    read_length_input.read(byte_ptr(&read_length), sizeof(uint16_t));
    position_input.read(byte_ptr(&read_position), sizeof(uint64_t));
    orientation_by_read[read_order] = orientation_char;
    read_lengths_by_read[read_order] = read_length;
    aligned_flags[read_order] = true;
    position_by_read[read_order] = read_position;
    noise_offset_by_read[read_order] = next_noise_offset;
    noise_count_for_read = 0;
    noise_input.get(noise_code);
    while (noise_input.good() && noise_code != '\n') {
      if (next_noise_offset >= noise_entry_count) {
        throw std::runtime_error("Corruption in noise stream: excess codes "
                                 "found beyond header limit.");
      }
      noise_codes[next_noise_offset++] = noise_code;
      noise_count_for_read++;
      if (!noise_input.get(noise_code))
        break;
    }
    for (uint16_t noise_index = 0; noise_index < noise_count_for_read;
         noise_index++) {
      noise_position_input.read(byte_ptr(&noise_position), sizeof(uint16_t));
      noise_positions[next_noise_position_offset] = noise_position;
      next_noise_position_offset++;
    }
    noise_count_by_read[read_order] = noise_count_for_read;
    aligned_read_count++;
    if (!(paired_end || preserve_order))
      read_order++;
  }
  noise_input.close();
  noise_position_input.close();
  orientation_input.close();
  position_input.close();

  unaligned_read_count = num_reads - aligned_read_count;
  std::string unaligned_count_path = paths.unaligned_path + ".count";
  std::ifstream unaligned_count_input(unaligned_count_path, std::ios::binary);
  uint64_t unaligned_char_count;
  unaligned_count_input.read(byte_ptr(&unaligned_char_count), sizeof(uint64_t));
  unaligned_count_input.close();
  remove(unaligned_count_path.c_str());
  std::vector<char> unaligned_chars(unaligned_char_count);
  std::ifstream unaligned_input(paths.unaligned_path, std::ios::binary);
  std::string unaligned_read;
  uint64_t next_unaligned_offset = 0;
  for (uint32_t read_index = 0; read_index < unaligned_read_count;
       read_index++) {
    read_dnaN_from_bits(unaligned_read, unaligned_input);
    std::memcpy(unaligned_chars.data() + next_unaligned_offset,
                &unaligned_read[0], unaligned_read.size());
    next_unaligned_offset += unaligned_read.size();
  }
  unaligned_input.close();
  uint64_t current_unaligned_offset = 0;
  for (uint32_t read_index = 0; read_index < unaligned_read_count;
       read_index++) {
    if (paired_end || preserve_order)
      order_input.read(byte_ptr(&read_order), sizeof(uint32_t));
    read_length_input.read(byte_ptr(&read_length), sizeof(uint16_t));
    read_lengths_by_read[read_order] = read_length;
    position_by_read[read_order] = current_unaligned_offset;
    current_unaligned_offset += read_length;
    aligned_flags[read_order] = false;
    if (!(paired_end || preserve_order))
      read_order++;
  }
  if (paired_end || preserve_order)
    order_input.close();
  read_length_input.close();

  remove_input_stream_files(paths);

  omp_set_num_threads(num_thr);
  const uint32_t num_reads_per_block = cp.num_reads_per_block;
  const temporary_stream_paths temp_paths =
      build_temporary_stream_paths(temp_dir);
  const uint64_t read_limit = paired_end ? half_read_count : num_reads;

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
          block_file_path(temp_paths.orientation_path, block_num), std::ios::binary);
      std::ofstream unaligned_output(
          block_file_path(temp_paths.unaligned_path, block_num), std::ios::binary);
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
            block_file_path(paths.mate_orientation_path, block_num), std::ios::binary);
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
              std::abs(mate_position_delta) < 32767)
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

  return;
}

} // namespace spring
