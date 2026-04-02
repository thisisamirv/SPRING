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

#include <cstdio>
#include <fstream>
#include <iostream>
#include <vector>

#include "paired_end_order.h"
#include "util.h"

namespace spring {

void pe_encode(const std::string &temp_dir, const compression_params &cp) {
  const uint32_t num_reads = cp.num_reads;
  const uint32_t half_read_count = num_reads / 2;

  const std::string base_dir = temp_dir;
  const std::string read_order_path = base_dir + "/read_order.bin";
  std::vector<uint32_t> reordered_positions(num_reads);
  std::vector<uint32_t> original_index_by_order(num_reads);

  std::ifstream order_input(read_order_path, std::ios::binary);
  uint32_t current_order;
  for (uint32_t read_index = 0; read_index < num_reads; read_index++) {
    order_input.read(byte_ptr(&current_order), sizeof(uint32_t));
    reordered_positions[read_index] = current_order;
    original_index_by_order[current_order] = read_index;
  }
  order_input.close();

  // File 1 keeps its reordered traversal; file 2 follows its mate positions.
  uint32_t next_first_mate_order = 0;
  for (uint32_t read_index = 0; read_index < num_reads; read_index++) {
    if (reordered_positions[read_index] < half_read_count)
      reordered_positions[read_index] = next_first_mate_order++;
  }

  for (uint32_t read_index = 0; read_index < num_reads; read_index++) {
    if (reordered_positions[read_index] >= half_read_count) {
      const uint32_t original_order = reordered_positions[read_index];
      const uint32_t mate_original_order = original_order - half_read_count;
      const uint32_t mate_read_index =
          original_index_by_order[mate_original_order];
      const uint32_t mate_reordered_position =
          reordered_positions[mate_read_index];
      reordered_positions[read_index] =
          mate_reordered_position + half_read_count;
    }
  }

  std::ofstream order_output(read_order_path + ".tmp", std::ios::binary);
  for (uint32_t read_index = 0; read_index < num_reads; read_index++) {
    order_output.write(byte_ptr(&reordered_positions[read_index]),
                       sizeof(uint32_t));
  }
  order_output.close();

  remove(read_order_path.c_str());
  rename((read_order_path + ".tmp").c_str(), read_order_path.c_str());
}

} // namespace spring
