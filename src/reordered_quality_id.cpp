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

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <omp.h>
#include <string>
#include <vector>

#include "libbsc/bsc.h"
#include "reordered_quality_id.h"
#include "util.h"

namespace spring {

namespace {

enum class reorder_compress_mode : uint8_t {
  quality,
  id,
};

struct batch_range {
  uint32_t begin;
  uint32_t end;
};

std::string block_file_path(const std::string &base_path,
                            const uint64_t block_num) {
  return base_path + "." + std::to_string(block_num);
}

uint32_t reads_per_file(const uint32_t num_reads,
                        const bool paired_end) {
  return paired_end ? num_reads / 2 : num_reads;
}

uint32_t compute_batch_size(const uint32_t num_reads,
                           const uint32_t num_reads_per_block) {
  return (1 + (num_reads / 4 - 1) / num_reads_per_block) * num_reads_per_block;
}

batch_range batch_read_range(const uint32_t batch_index,
                             const uint32_t batch_size,
                             const uint32_t total_reads) {
  const uint32_t batch_begin = batch_index * batch_size;
  const uint32_t batch_end = std::min(batch_begin + batch_size, total_reads);
  return {batch_begin, batch_end};
}

uint32_t block_read_count(const uint64_t block_begin,
                          const uint64_t block_end) {
  return static_cast<uint32_t>(block_end - block_begin);
}

void load_reordered_batch(const std::string &input_path,
                          const std::vector<uint32_t> &reordered_positions,
                          const batch_range &batch,
                          std::vector<std::string> &reordered_strings) {
  std::ifstream input_stream(input_path);
  std::string current_string;
  for (uint32_t read_index = 0; read_index < reordered_positions.size();
       read_index++) {
    std::getline(input_stream, current_string);
    if (reordered_positions[read_index] >= batch.begin &&
        reordered_positions[read_index] < batch.end) {
      reordered_strings[reordered_positions[read_index] - batch.begin] =
          current_string;
    }
  }
}

void compress_block_batch(const std::string &input_path,
                          const reorder_compress_mode mode,
                          const compression_params &compression_params,
                          std::vector<std::string> &reordered_strings,
                          const batch_range &batch,
                          const uint32_t num_reads_per_block,
                          const int num_threads) {
#pragma omp parallel
  {
    const uint64_t thread_id = omp_get_thread_num();
    const uint64_t block_offset = batch.begin / num_reads_per_block;
    uint64_t block_index = thread_id;
    std::vector<uint32_t> read_lengths;
    if (mode == reorder_compress_mode::quality)
      read_lengths.resize(num_reads_per_block);

    while (true) {
      const uint64_t block_begin = block_index * num_reads_per_block;
      if (block_begin >= batch.end - batch.begin)
        break;

      uint64_t block_end = (block_index + 1) * num_reads_per_block;
      if (block_end > batch.end - batch.begin)
        block_end = batch.end - batch.begin;

      const uint32_t reads_in_block = block_read_count(block_begin, block_end);
      const std::string output_path =
          block_file_path(input_path, block_offset + block_index);

      if (mode == reorder_compress_mode::id) {
        compress_id_block(output_path.c_str(),
                          reordered_strings.data() + block_begin,
                          reads_in_block);
      } else {
        for (uint64_t read_offset = 0; read_offset < reads_in_block;
             read_offset++) {
          read_lengths[read_offset] =
              reordered_strings[block_begin + read_offset].size();
        }
        if (compression_params.qvz_flag) {
          quantize_quality_qvz(reordered_strings.data() + block_begin,
                               reads_in_block, read_lengths.data(),
                               compression_params.qvz_ratio);
        }
        bsc::BSC_str_array_compress(output_path.c_str(),
                                    reordered_strings.data() + block_begin,
                                    reads_in_block, read_lengths.data());
      }

      block_index += num_threads;
    }
  }
}

bool should_process_stream(const int stream_index, const bool paired_end,
                           const bool skip_second_stream) {
  if (!paired_end && stream_index == 1)
    return false;
  if (skip_second_stream && stream_index == 1)
    return false;
  return true;
}

void generate_order_pe(const std::string &read_order_path,
                       std::vector<uint32_t> &reordered_positions,
                       const uint32_t num_reads) {
  std::ifstream order_input(read_order_path, std::ios::binary);
  uint32_t current_order;
  uint32_t next_reordered_position = 0;
  const uint32_t half_read_count = num_reads / 2;
  for (uint32_t read_index = 0; read_index < num_reads; read_index++) {
    order_input.read(byte_ptr(&current_order), sizeof(uint32_t));
    if (current_order < half_read_count)
      reordered_positions[current_order] = next_reordered_position++;
  }
}

void generate_order_se(const std::string &read_order_path,
                       std::vector<uint32_t> &reordered_positions,
                       const uint32_t num_reads) {
  std::ifstream order_input(read_order_path, std::ios::binary);
  uint32_t current_order;
  for (uint32_t read_index = 0; read_index < num_reads; read_index++) {
    order_input.read(byte_ptr(&current_order), sizeof(uint32_t));
    reordered_positions[current_order] = read_index;
  }
}

void reorder_compress(const std::string &input_path,
                      const uint32_t num_reads_per_file, const int num_thr,
                      const uint32_t num_reads_per_block,
                      std::vector<std::string> &reordered_strings,
                      const uint32_t batch_size,
                      const std::vector<uint32_t> &reordered_positions,
                      const reorder_compress_mode mode,
                      const compression_params &cp) {
  for (uint32_t batch_index = 0;; batch_index++) {
    const batch_range batch =
        batch_read_range(batch_index, batch_size, num_reads_per_file);
    if (batch.begin >= batch.end)
      break;

    load_reordered_batch(input_path, reordered_positions, batch,
                         reordered_strings);
    compress_block_batch(input_path, mode, cp, reordered_strings, batch,
                         num_reads_per_block, num_thr);
  }
}

} // namespace

void reorder_compress_quality_id(const std::string &temp_dir,
                                 const compression_params &cp) {
  const uint32_t num_reads = cp.num_reads;
  const int num_thr = cp.num_thr;
  const bool preserve_id = cp.preserve_id;
  const bool preserve_quality = cp.preserve_quality;
  const bool paired_end = cp.paired_end;
  const uint32_t num_reads_per_block = cp.num_reads_per_block;
  const bool paired_id_match = cp.paired_id_match;

  const std::string base_dir = temp_dir;

  const std::string read_order_path = base_dir + "/read_order.bin";
  std::string id_paths[2];
  std::string quality_paths[2];
  id_paths[0] = base_dir + "/id_1";
  id_paths[1] = base_dir + "/id_2";
  quality_paths[0] = base_dir + "/quality_1";
  quality_paths[1] = base_dir + "/quality_2";

  std::vector<uint32_t> reordered_positions;
  if (paired_end) {
    reordered_positions.resize(num_reads / 2);
    generate_order_pe(read_order_path, reordered_positions, num_reads);
  } else {
    reordered_positions.resize(num_reads);
    generate_order_se(read_order_path, reordered_positions, num_reads);
  }

  omp_set_num_threads(num_thr);

  const uint32_t batch_size =
      compute_batch_size(num_reads, num_reads_per_block);
  std::vector<std::string> reordered_strings(batch_size);
  // Bound the working set so this stage stays within the reorder memory budget.

  if (preserve_quality) {
    std::cout << "Compressing qualities\n";
    for (int stream_index = 0; stream_index < 2; stream_index++) {
      if (!should_process_stream(stream_index, paired_end, false))
        continue;
      const uint32_t file_read_count = reads_per_file(num_reads, paired_end);
      reorder_compress(quality_paths[stream_index], file_read_count, num_thr,
                       num_reads_per_block, reordered_strings, batch_size,
                       reordered_positions, reorder_compress_mode::quality, cp);
      remove(quality_paths[stream_index].c_str());
    }
  }
  if (preserve_id) {
    std::cout << "Compressing ids\n";
    for (int stream_index = 0; stream_index < 2; stream_index++) {
      if (!should_process_stream(stream_index, paired_end, paired_id_match))
        continue;
      const uint32_t file_read_count = reads_per_file(num_reads, paired_end);
      reorder_compress(id_paths[stream_index], file_read_count, num_thr,
                       num_reads_per_block, reordered_strings, batch_size,
                       reordered_positions, reorder_compress_mode::id, cp);
      remove(id_paths[stream_index].c_str());
    }
  }
}

} // namespace spring
