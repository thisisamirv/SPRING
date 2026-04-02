/*
* Copyright 2018 University of Illinois Board of Trustees and Stanford
University. All Rights Reserved.
* Licensed under the “Non-exclusive Research Use License for SPRING Software”
license (the "License");
* You may not use this file except in compliance with the License.
* The License is included in the distribution as license.pdf file.

* Software distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
limitations under the License.
*/

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

} // namespace

void reorder_compress_streams(const std::string &temp_dir,
                              const compression_params &cp) {
  const std::string base_dir = temp_dir;
  const std::string flag_path = base_dir + "/read_flag.txt";
  const std::string position_path = base_dir + "/read_pos.bin";
  const std::string mate_position_path = base_dir + "/read_pos_pair.bin";
  const std::string orientation_path = base_dir + "/read_rev.txt";
  const std::string mate_orientation_path = base_dir + "/read_rev_pair.txt";
  const std::string read_length_path = base_dir + "/read_lengths.bin";
  const std::string unaligned_path = base_dir + "/read_unaligned.txt";
  const std::string noise_path = base_dir + "/read_noise.txt";
  const std::string noise_position_path = base_dir + "/read_noisepos.bin";
  const std::string order_path = base_dir + "/read_order.bin";

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
    order_input.open(order_path, std::ios::binary);
  std::ifstream orientation_input(orientation_path);
  std::ifstream read_length_input(read_length_path, std::ios::binary);
  std::ifstream noise_input(noise_path);
  std::ifstream noise_position_input(noise_position_path, std::ios::binary);
  std::ifstream position_input(position_path, std::ios::binary);
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
    while (noise_code != '\n') {
      noise_codes[next_noise_offset++] = noise_code;
      noise_count_for_read++;
      noise_input.get(noise_code);
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
  std::string unaligned_count_path = unaligned_path + ".count";
  std::ifstream unaligned_count_input(unaligned_count_path);
  uint64_t unaligned_char_count;
  unaligned_count_input.read(byte_ptr(&unaligned_char_count), sizeof(uint64_t));
  unaligned_count_input.close();
  remove(unaligned_count_path.c_str());
  std::vector<char> unaligned_chars(unaligned_char_count);
  std::ifstream unaligned_input(unaligned_path, std::ios::binary);
  std::string unaligned_read;
  uint64_t next_unaligned_offset = 0;
  for (uint32_t read_index = 0; read_index < unaligned_read_count;
       read_index++) {
    read_dnaN_from_bits(unaligned_read, unaligned_input);
    std::memcpy(unaligned_chars.data() + next_unaligned_offset,
                &unaligned_read[0],
                unaligned_read.size());
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

  remove(noise_path.c_str());
  remove(noise_position_path.c_str());
  remove(orientation_path.c_str());
  remove(order_path.c_str());
  remove(read_length_path.c_str());
  remove(unaligned_path.c_str());
  remove(position_path.c_str());

  omp_set_num_threads(num_thr);
  uint32_t num_reads_per_block = cp.num_reads_per_block;

  const std::string temp_position_path = base_dir + "/a";
  const std::string temp_noise_path = base_dir + "/b";
  const std::string temp_noise_position_path = base_dir + "/c";
  const std::string temp_orientation_path = base_dir + "/d";
  const std::string temp_flag_path = base_dir + "/e";
  const std::string temp_unaligned_path = base_dir + "/f";
  const std::string temp_read_length_path = base_dir + "/g";

  // In paired-end mode, the block indices count read pairs rather than reads.
#pragma omp parallel
  {
    uint64_t thread_id = omp_get_thread_num();
    uint64_t block_num = thread_id;
    bool block_range_done = false;
    while (!block_range_done) {
      uint64_t start_read_index = block_num * num_reads_per_block;
      uint64_t end_read_index = (block_num + 1) * num_reads_per_block;
      if (!paired_end) {
        if (start_read_index >= num_reads)
          break;
        if (end_read_index >= num_reads) {
          block_range_done = true;
          end_read_index = num_reads;
        }
      } else {
        if (start_read_index >= half_read_count)
          break;
        if (end_read_index >= half_read_count) {
          block_range_done = true;
          end_read_index = half_read_count;
        }
      }
      std::ofstream flag_output(block_file_path(temp_flag_path, block_num));
      std::ofstream noise_output(block_file_path(temp_noise_path, block_num));
      std::ofstream noise_position_output(
          block_file_path(temp_noise_position_path, block_num),
          std::ios::binary);
      std::ofstream position_output(block_file_path(temp_position_path, block_num),
                                    std::ios::binary);
      std::ofstream orientation_output(
          block_file_path(temp_orientation_path, block_num));
      std::ofstream unaligned_output(
          block_file_path(temp_unaligned_path, block_num));
      std::ofstream read_length_output(
          block_file_path(temp_read_length_path, block_num), std::ios::binary);
      std::ofstream mate_position_output;
      std::ofstream mate_orientation_output;
      if (paired_end) {
        mate_position_output.open(block_file_path(mate_position_path, block_num),
                                  std::ios::binary);
        mate_orientation_output.open(
            block_file_path(mate_orientation_path, block_num));
      }

      uint64_t previous_position = 0;
      uint64_t position_delta;
      uint16_t position_delta_16;
      for (uint64_t read_index = start_read_index;
           read_index < end_read_index; read_index++) {
        if (!paired_end) {
          read_length_output.write(byte_ptr(&read_lengths_by_read[read_index]),
                                   sizeof(uint16_t));
          if (aligned_flags[read_index] == true) {
            flag_output << '0';
            orientation_output << orientation_by_read[read_index];
            if (preserve_order)
              position_output.write(byte_ptr(&position_by_read[read_index]),
                                    sizeof(uint64_t));
            else {
              if (read_index == start_read_index) {
                // Each non-preserving block starts with one absolute anchor.
                position_output.write(byte_ptr(&position_by_read[read_index]),
                                      sizeof(uint64_t));
                previous_position = position_by_read[read_index];
              } else {
                position_delta = position_by_read[read_index] - previous_position;
                if (position_delta < 65535) {
                  position_delta_16 = (uint16_t)position_delta;
                  position_output.write(byte_ptr(&position_delta_16),
                                        sizeof(uint16_t));
                } else {
                  position_delta_16 = 65535;
                  position_output.write(byte_ptr(&position_delta_16),
                                        sizeof(uint16_t));
                  position_output.write(byte_ptr(&position_by_read[read_index]),
                                        sizeof(uint64_t));
                }
                previous_position = position_by_read[read_index];
              }
            }
            for (uint16_t noise_index = 0;
                 noise_index < noise_count_by_read[read_index]; noise_index++) {
              noise_output <<
                  noise_codes[noise_offset_by_read[read_index] + noise_index];
              noise_position_output.write(
                  byte_ptr(&noise_positions[noise_offset_by_read[read_index] +
                                            noise_index]),
                  sizeof(uint16_t));
            }
            noise_output << "\n";
          } else {
            flag_output << '2';
            unaligned_output.write(unaligned_chars.data() + position_by_read[read_index],
                                   read_lengths_by_read[read_index]);
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
          else if (!aligned_flags[read_index] && !aligned_flags[mate_read_index])
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
            if (preserve_order)
              position_output.write(byte_ptr(&position_by_read[read_index]),
                                    sizeof(uint64_t));
            else {
              if (read_index == start_read_index) {
                position_output.write(byte_ptr(&position_by_read[read_index]),
                                      sizeof(uint64_t));
                previous_position = position_by_read[read_index];
              } else {
                position_delta = position_by_read[read_index] - previous_position;
                if (position_delta < 65535) {
                  position_delta_16 = (uint16_t)position_delta;
                  position_output.write(byte_ptr(&position_delta_16),
                                        sizeof(uint16_t));
                } else {
                  position_delta_16 = 65535;
                  position_output.write(byte_ptr(&position_delta_16),
                                        sizeof(uint16_t));
                  position_output.write(byte_ptr(&position_by_read[read_index]),
                                        sizeof(uint64_t));
                }
                previous_position = position_by_read[read_index];
              }
            }
            for (uint16_t noise_index = 0;
                 noise_index < noise_count_by_read[read_index]; noise_index++) {
              noise_output <<
                  noise_codes[noise_offset_by_read[read_index] + noise_index];
              noise_position_output.write(
                  byte_ptr(&noise_positions[noise_offset_by_read[read_index] +
                                            noise_index]),
                  sizeof(uint16_t));
            }
            noise_output << "\n";
            orientation_output << orientation_by_read[read_index];
          } else {
            unaligned_output.write(unaligned_chars.data() + position_by_read[read_index],
                                   read_lengths_by_read[read_index]);
          }

          if (read_flag == 0 || read_flag == 1 || read_flag == 4) {
            for (uint16_t noise_index = 0;
                 noise_index < noise_count_by_read[mate_read_index];
                 noise_index++) {
              noise_output << noise_codes[noise_offset_by_read[mate_read_index] +
                                          noise_index];
              noise_position_output.write(
                  byte_ptr(&noise_positions[noise_offset_by_read[mate_read_index] +
                                            noise_index]),
                  sizeof(uint16_t));
            }
            noise_output << "\n";
            if (read_flag == 1 || read_flag == 4) {
              position_output.write(byte_ptr(&position_by_read[mate_read_index]),
                                    sizeof(uint64_t));
              orientation_output << orientation_by_read[mate_read_index];
            }
          } else {
            unaligned_output.write(
                unaligned_chars.data() + position_by_read[mate_read_index],
                read_lengths_by_read[mate_read_index]);
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

      compress_temp_block(temp_flag_path, flag_path, block_num);
      compress_temp_block(temp_position_path, position_path, block_num);
      compress_temp_block(temp_noise_path, noise_path, block_num);
      compress_temp_block(temp_noise_position_path, noise_position_path,
                          block_num);
      compress_temp_block(temp_unaligned_path, unaligned_path, block_num);
      compress_temp_block(temp_read_length_path, read_length_path, block_num);
      compress_temp_block(temp_orientation_path, orientation_path, block_num);

      if (paired_end) {
        compress_block_file(block_file_path(mate_position_path, block_num),
                            compressed_block_file_path(mate_position_path,
                                                       block_num));
        compress_block_file(block_file_path(mate_orientation_path, block_num),
                            compressed_block_file_path(mate_orientation_path,
                                                       block_num));
      }

      block_num += num_thr;
    }
  }

  return;
}

} // namespace spring
