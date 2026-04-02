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
#include "reorder_compress_quality_id.h"
#include "util.h"

namespace spring {

namespace {

enum class reorder_compress_mode : uint8_t {
  quality,
  id,
};

std::string block_file_path(const std::string &base_path,
                            const uint64_t block_num) {
  return base_path + "." + std::to_string(block_num);
}

uint32_t reads_per_file(const uint32_t numreads, const bool paired_end) {
  return paired_end ? numreads / 2 : numreads;
}

void generate_order_pe(const std::string &file_order,
                       std::vector<uint32_t> &order_array,
                       const uint32_t numreads) {
  std::ifstream fin_order(file_order, std::ios::binary);
  uint32_t order;
  uint32_t pos_after_reordering = 0;
  const uint32_t numreads_by_2 = numreads / 2;
  for (uint32_t i = 0; i < numreads; i++) {
    fin_order.read(byte_ptr(&order), sizeof(uint32_t));
    if (order < numreads_by_2)
      order_array[order] = pos_after_reordering++;
  }
}

void generate_order_se(const std::string &file_order,
                       std::vector<uint32_t> &order_array,
                       const uint32_t numreads) {
  std::ifstream fin_order(file_order, std::ios::binary);
  uint32_t order;
  for (uint32_t i = 0; i < numreads; i++) {
    fin_order.read(byte_ptr(&order), sizeof(uint32_t));
    order_array[order] = i;
  }
}

void reorder_compress(const std::string &file_name,
                      const uint32_t num_reads_per_file, const int num_thr,
                      const uint32_t num_reads_per_block,
                      std::vector<std::string> &str_array,
                      const uint32_t str_array_size,
                      const std::vector<uint32_t> &order_array,
                      const reorder_compress_mode mode,
                      const compression_params &cp) {
  for (uint32_t bin_index = 0; bin_index <= num_reads_per_file / str_array_size;
       bin_index++) {
    uint32_t num_reads_bin = str_array_size;
    if (bin_index == num_reads_per_file / str_array_size)
      num_reads_bin = num_reads_per_file % str_array_size;
    if (num_reads_bin == 0)
      break;

    const uint32_t start_read_bin = bin_index * str_array_size;
    const uint32_t end_read_bin = start_read_bin + num_reads_bin;

    std::ifstream f_in(file_name);
    std::string temp_str;
    for (uint32_t read_index = 0; read_index < num_reads_per_file; read_index++) {
      std::getline(f_in, temp_str);
      if (order_array[read_index] >= start_read_bin &&
          order_array[read_index] < end_read_bin) {
        str_array[order_array[read_index] - start_read_bin] = temp_str;
      }
    }
    f_in.close();

#pragma omp parallel
    {
      const uint64_t tid = omp_get_thread_num();
      const uint64_t block_num_offset = start_read_bin / num_reads_per_block;
      uint64_t block_num = tid;
      std::vector<uint32_t> read_lengths_array;
      if (mode == reorder_compress_mode::quality)
        read_lengths_array.resize(num_reads_per_block);

      bool done = false;
      while (!done) {
        const uint64_t start_read_num = block_num * num_reads_per_block;
        uint64_t end_read_num = (block_num + 1) * num_reads_per_block;
        if (start_read_num >= num_reads_bin)
          break;
        if (end_read_num >= num_reads_bin) {
          done = true;
          end_read_num = num_reads_bin;
        }

        const uint32_t num_reads_block =
            static_cast<uint32_t>(end_read_num - start_read_num);
        const std::string outfile_name =
            block_file_path(file_name, block_num_offset + block_num);

        if (mode == reorder_compress_mode::id) {
          compress_id_block(outfile_name.c_str(), str_array.data() + start_read_num,
                            num_reads_block);
        } else {
          for (uint64_t i = 0; i < num_reads_block; i++)
            read_lengths_array[i] = str_array[start_read_num + i].size();
          if (cp.qvz_flag) {
            quantize_quality_qvz(str_array.data() + start_read_num,
                                 num_reads_block, read_lengths_array.data(),
                                 cp.qvz_ratio);
          }
          bsc::BSC_str_array_compress(outfile_name.c_str(),
                                      str_array.data() + start_read_num,
                                      num_reads_block, read_lengths_array.data());
        }
        block_num += num_thr;
      }
    } // omp parallel
  }
}

} // namespace

void reorder_compress_quality_id(const std::string &temp_dir,
                                 const compression_params &cp) {
  const uint32_t numreads = cp.num_reads;
  const int num_thr = cp.num_thr;
  const bool preserve_id = cp.preserve_id;
  const bool preserve_quality = cp.preserve_quality;
  const bool paired_end = cp.paired_end;
  const uint32_t num_reads_per_block = cp.num_reads_per_block;
  const bool paired_id_match = cp.paired_id_match;

  const std::string basedir = temp_dir;

  const std::string file_order = basedir + "/read_order.bin";
  std::string file_id[2];
  std::string file_quality[2];
  file_id[0] = basedir + "/id_1";
  file_id[1] = basedir + "/id_2";
  file_quality[0] = basedir + "/quality_1";
  file_quality[1] = basedir + "/quality_2";

  std::vector<uint32_t> order_array;
  if (paired_end) {
    order_array.resize(numreads / 2);
    generate_order_pe(file_order, order_array, numreads);
  } else {
    order_array.resize(numreads);
    generate_order_se(file_order, order_array, numreads);
  }

  omp_set_num_threads(num_thr);

  const uint32_t str_array_size =
      (1 + (numreads / 4 - 1) / num_reads_per_block) * num_reads_per_block;
  std::vector<std::string> str_array(str_array_size);
  // Bound the working set so this stage stays within the reorder memory budget.

  if (preserve_quality) {
    std::cout << "Compressing qualities\n";
    for (int j = 0; j < 2; j++) {
      if (!paired_end && j == 1)
        break;
      const uint32_t num_reads_per_file = reads_per_file(numreads, paired_end);
      reorder_compress(file_quality[j], num_reads_per_file, num_thr,
                       num_reads_per_block, str_array, str_array_size,
                       order_array, reorder_compress_mode::quality, cp);
      remove(file_quality[j].c_str());
    }
  }
  if (preserve_id) {
    std::cout << "Compressing ids\n";
    for (int j = 0; j < 2; j++) {
      if (!paired_end && j == 1)
        break;
      if (j == 1 && paired_id_match)
        break;
      const uint32_t num_reads_per_file = reads_per_file(numreads, paired_end);
      reorder_compress(file_id[j], num_reads_per_file, num_thr,
                       num_reads_per_block, str_array, str_array_size,
                       order_array, reorder_compress_mode::id, cp);
      remove(file_id[j].c_str());
    }
  }
}

} // namespace spring
