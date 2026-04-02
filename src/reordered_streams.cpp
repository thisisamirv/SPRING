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
  const std::string basedir = temp_dir;
  const std::string file_flag = basedir + "/read_flag.txt";
  const std::string file_pos = basedir + "/read_pos.bin";
  const std::string file_pos_pair = basedir + "/read_pos_pair.bin";
  const std::string file_RC = basedir + "/read_rev.txt";
  const std::string file_RC_pair = basedir + "/read_rev_pair.txt";
  const std::string file_readlength = basedir + "/read_lengths.bin";
  const std::string file_unaligned = basedir + "/read_unaligned.txt";
  const std::string file_noise = basedir + "/read_noise.txt";
  const std::string file_noisepos = basedir + "/read_noisepos.bin";
  const std::string file_order = basedir + "/read_order.bin";

  uint32_t num_reads = cp.num_reads, num_reads_aligned = 0, num_reads_unaligned;
  const uint32_t num_reads_by_2 = num_reads / 2;
  const int num_thr = cp.num_thr;
  const bool paired_end = cp.paired_end;
  const bool preserve_order = cp.preserve_order;

  std::vector<char> RC_arr(num_reads);
  std::vector<uint16_t> read_length_arr(num_reads);
  std::vector<bool> flag_arr(num_reads);
  std::vector<uint64_t> pos_in_noise_arr(num_reads);
  std::vector<uint64_t> pos_arr(num_reads);
  std::vector<uint16_t> noise_len_arr(num_reads);

  std::ifstream f_order;
  if (paired_end || preserve_order)
    f_order.open(file_order, std::ios::binary);
  std::ifstream f_RC(file_RC);
  std::ifstream f_readlength(file_readlength, std::ios::binary);
  std::ifstream f_noise(file_noise);
  std::ifstream f_noisepos(file_noisepos, std::ios::binary);
  std::ifstream f_pos(file_pos, std::ios::binary);
  f_noisepos.seekg(0, f_noisepos.end);
  uint64_t noise_array_size = f_noisepos.tellg() / 2;
  f_noisepos.seekg(0, f_noisepos.beg);
  std::vector<char> noise_arr(noise_array_size);
  std::vector<uint16_t> noisepos_arr(noise_array_size);
  char rc, noise_char;
  uint32_t order = 0;
  uint64_t current_pos_noise_arr = 0;
  uint64_t current_pos_noisepos_arr = 0;
  uint64_t pos;
  uint16_t num_noise_in_curr_read;
  uint16_t read_length, noisepos;

  while (f_RC.get(rc)) {
    if (paired_end || preserve_order)
      f_order.read(byte_ptr(&order), sizeof(uint32_t));
    f_readlength.read(byte_ptr(&read_length), sizeof(uint16_t));
    f_pos.read(byte_ptr(&pos), sizeof(uint64_t));
    RC_arr[order] = rc;
    read_length_arr[order] = read_length;
    flag_arr[order] = true;
    pos_arr[order] = pos;
    pos_in_noise_arr[order] = current_pos_noise_arr;
    num_noise_in_curr_read = 0;
    f_noise.get(noise_char);
    while (noise_char != '\n') {
      noise_arr[current_pos_noise_arr++] = noise_char;
      num_noise_in_curr_read++;
      f_noise.get(noise_char);
    }
    for (uint16_t i = 0; i < num_noise_in_curr_read; i++) {
      f_noisepos.read(byte_ptr(&noisepos), sizeof(uint16_t));
      noisepos_arr[current_pos_noisepos_arr] = noisepos;
      current_pos_noisepos_arr++;
    }
    noise_len_arr[order] = num_noise_in_curr_read;
    num_reads_aligned++;
    if (!(paired_end || preserve_order))
      order++;
  }
  f_noise.close();
  f_noisepos.close();
  f_RC.close();
  f_pos.close();

  num_reads_unaligned = num_reads - num_reads_aligned;
  std::string file_unaligned_count = file_unaligned + ".count";
  std::ifstream f_unaligned_count(file_unaligned_count);
  uint64_t unaligned_array_size;
  f_unaligned_count.read(byte_ptr(&unaligned_array_size), sizeof(uint64_t));
  f_unaligned_count.close();
  remove(file_unaligned_count.c_str());
  std::vector<char> unaligned_arr(unaligned_array_size);
  std::ifstream f_unaligned(file_unaligned, std::ios::binary);
  std::string unaligned_read;
  uint64_t pos_in_unaligned_arr = 0;
  for (uint32_t i = 0; i < num_reads_unaligned; i++) {
    read_dnaN_from_bits(unaligned_read, f_unaligned);
    std::memcpy(unaligned_arr.data() + pos_in_unaligned_arr, &unaligned_read[0],
                unaligned_read.size());
    pos_in_unaligned_arr += unaligned_read.size();
  }
  f_unaligned.close();
  uint64_t current_pos_in_unaligned_arr = 0;
  for (uint32_t i = 0; i < num_reads_unaligned; i++) {
    if (paired_end || preserve_order)
      f_order.read(byte_ptr(&order), sizeof(uint32_t));
    f_readlength.read(byte_ptr(&read_length), sizeof(uint16_t));
    read_length_arr[order] = read_length;
    pos_arr[order] = current_pos_in_unaligned_arr;
    current_pos_in_unaligned_arr += read_length;
    flag_arr[order] = false;
    if (!(paired_end || preserve_order))
      order++;
  }
  if (paired_end || preserve_order)
    f_order.close();
  f_readlength.close();

  remove(file_noise.c_str());
  remove(file_noisepos.c_str());
  remove(file_RC.c_str());
  remove(file_order.c_str());
  remove(file_readlength.c_str());
  remove(file_unaligned.c_str());
  remove(file_pos.c_str());

  omp_set_num_threads(num_thr);
  uint32_t num_reads_per_block = cp.num_reads_per_block;

  const std::string tmpfile_pos = basedir + "/a";
  const std::string tmpfile_noise = basedir + "/b";
  const std::string tmpfile_noisepos = basedir + "/c";
  const std::string tmpfile_RC = basedir + "/d";
  const std::string tmpfile_flag = basedir + "/e";
  const std::string tmpfile_unaligned = basedir + "/f";
  const std::string tmpfile_readlength = basedir + "/g";

  // In paired-end mode, the block indices count read pairs rather than reads.
#pragma omp parallel
  {
    uint64_t tid = omp_get_thread_num();
    uint64_t block_num = tid;
    bool done = false;
    while (!done) {
      uint64_t start_read_num = block_num * num_reads_per_block;
      uint64_t end_read_num = (block_num + 1) * num_reads_per_block;
      if (!paired_end) {
        if (start_read_num >= num_reads)
          break;
        if (end_read_num >= num_reads) {
          done = true;
          end_read_num = num_reads;
        }
      } else {
        if (start_read_num >= num_reads_by_2)
          break;
        if (end_read_num >= num_reads_by_2) {
          done = true;
          end_read_num = num_reads_by_2;
        }
      }
      std::ofstream f_flag(block_file_path(tmpfile_flag, block_num));
      std::ofstream f_noise(block_file_path(tmpfile_noise, block_num));
      std::ofstream f_noisepos(block_file_path(tmpfile_noisepos, block_num),
                               std::ios::binary);
      std::ofstream f_pos(block_file_path(tmpfile_pos, block_num),
                          std::ios::binary);
      std::ofstream f_RC(block_file_path(tmpfile_RC, block_num));
      std::ofstream f_unaligned(block_file_path(tmpfile_unaligned, block_num));
      std::ofstream f_readlength(block_file_path(tmpfile_readlength, block_num),
                                 std::ios::binary);
      std::ofstream f_pos_pair;
      std::ofstream f_RC_pair;
      if (paired_end) {
        f_pos_pair.open(block_file_path(file_pos_pair, block_num),
                        std::ios::binary);
        f_RC_pair.open(block_file_path(file_RC_pair, block_num));
      }

      uint64_t prevpos = 0, diffpos;
      uint16_t diffpos_16;
      for (uint64_t i = start_read_num; i < end_read_num; i++) {
        if (!paired_end) {
          f_readlength.write(byte_ptr(&read_length_arr[i]), sizeof(uint16_t));
          if (flag_arr[i] == true) {
            f_flag << '0';
            f_RC << RC_arr[i];
            if (preserve_order)
              f_pos.write(byte_ptr(&pos_arr[i]), sizeof(uint64_t));
            else {
              if (i == start_read_num) {
                // Each non-preserving block starts with one absolute anchor.
                f_pos.write(byte_ptr(&pos_arr[i]), sizeof(uint64_t));
                prevpos = pos_arr[i];
              } else {
                diffpos = pos_arr[i] - prevpos;
                if (diffpos < 65535) {
                  diffpos_16 = (uint16_t)diffpos;
                  f_pos.write(byte_ptr(&diffpos_16), sizeof(uint16_t));
                } else {
                  diffpos_16 = 65535;
                  f_pos.write(byte_ptr(&diffpos_16), sizeof(uint16_t));
                  f_pos.write(byte_ptr(&pos_arr[i]), sizeof(uint64_t));
                }
                prevpos = pos_arr[i];
              }
            }
            for (uint16_t j = 0; j < noise_len_arr[i]; j++) {
              f_noise << noise_arr[pos_in_noise_arr[i] + j];
              f_noisepos.write(byte_ptr(&noisepos_arr[pos_in_noise_arr[i] + j]),
                               sizeof(uint16_t));
            }
            f_noise << "\n";
          } else {
            f_flag << '2';
            f_unaligned.write(unaligned_arr.data() + pos_arr[i],
                              read_length_arr[i]);
          }
        } else {
          uint64_t i_p = num_reads_by_2 + i;
          f_readlength.write(byte_ptr(&read_length_arr[i]), sizeof(uint16_t));
          f_readlength.write(byte_ptr(&read_length_arr[i_p]), sizeof(uint16_t));
          int64_t pos_pair = (int64_t)pos_arr[i_p] - (int64_t)pos_arr[i];
          int flag = 2;
          if (flag_arr[i] && flag_arr[i_p] && std::abs(pos_pair) < 32767)
            flag = 0;
          else if (flag_arr[i] && flag_arr[i_p])
            flag = 1;
          else if (!flag_arr[i] && !flag_arr[i_p])
            flag = 2;
          else if (flag_arr[i] && !flag_arr[i_p])
            flag = 3;
          else if (!flag_arr[i] && flag_arr[i_p])
            flag = 4;
          f_flag << flag;
          if (flag == 0 && paired_end) {
            int16_t pos_pair_16 = (int16_t)pos_pair;
            f_pos_pair.write(byte_ptr(&pos_pair_16), sizeof(int16_t));
            if (RC_arr[i] != RC_arr[i_p])
              f_RC_pair << '0';
            else
              f_RC_pair << '1';
          }
          if (flag == 0 || flag == 1 || flag == 3) {
            // read 1 is aligned
            if (preserve_order)
              f_pos.write(byte_ptr(&pos_arr[i]), sizeof(uint64_t));
            else {
              if (i == start_read_num) {
                f_pos.write(byte_ptr(&pos_arr[i]), sizeof(uint64_t));
                prevpos = pos_arr[i];
              } else {
                diffpos = pos_arr[i] - prevpos;
                if (diffpos < 65535) {
                  diffpos_16 = (uint16_t)diffpos;
                  f_pos.write(byte_ptr(&diffpos_16), sizeof(uint16_t));
                } else {
                  diffpos_16 = 65535;
                  f_pos.write(byte_ptr(&diffpos_16), sizeof(uint16_t));
                  f_pos.write(byte_ptr(&pos_arr[i]), sizeof(uint64_t));
                }
                prevpos = pos_arr[i];
              }
            }
            for (uint16_t j = 0; j < noise_len_arr[i]; j++) {
              f_noise << noise_arr[pos_in_noise_arr[i] + j];
              f_noisepos.write(byte_ptr(&noisepos_arr[pos_in_noise_arr[i] + j]),
                               sizeof(uint16_t));
            }
            f_noise << "\n";
            f_RC << RC_arr[i];
          } else {
            f_unaligned.write(unaligned_arr.data() + pos_arr[i],
                              read_length_arr[i]);
          }

          if (flag == 0 || flag == 1 || flag == 4) {
            for (uint16_t j = 0; j < noise_len_arr[i_p]; j++) {
              f_noise << noise_arr[pos_in_noise_arr[i_p] + j];
              f_noisepos.write(byte_ptr(&noisepos_arr[pos_in_noise_arr[i_p] + j]),
                               sizeof(uint16_t));
            }
            f_noise << "\n";
            if (flag == 1 || flag == 4) {
              f_pos.write(byte_ptr(&pos_arr[i_p]), sizeof(uint64_t));
              f_RC << RC_arr[i_p];
            }
          } else {
            f_unaligned.write(unaligned_arr.data() + pos_arr[i_p],
                              read_length_arr[i_p]);
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

      compress_temp_block(tmpfile_flag, file_flag, block_num);
      compress_temp_block(tmpfile_pos, file_pos, block_num);
      compress_temp_block(tmpfile_noise, file_noise, block_num);
      compress_temp_block(tmpfile_noisepos, file_noisepos, block_num);
      compress_temp_block(tmpfile_unaligned, file_unaligned, block_num);
      compress_temp_block(tmpfile_readlength, file_readlength, block_num);
      compress_temp_block(tmpfile_RC, file_RC, block_num);

      if (paired_end) {
        compress_block_file(block_file_path(file_pos_pair, block_num),
                            compressed_block_file_path(file_pos_pair, block_num));
        compress_block_file(block_file_path(file_RC_pair, block_num),
                            compressed_block_file_path(file_RC_pair, block_num));
      }

      block_num += num_thr;
    }
  }

  return;
}

} // namespace spring
