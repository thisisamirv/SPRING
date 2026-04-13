// Reorders paired-end mate indices to match Spring's reordered read layout
// while preserving pair relationships for later reconstruction.

#include <fstream>
#include <iostream>
#include <vector>

#include "core_utils.h"
#include "fs_utils.h"
#include "paired_end_order.h"
#include "params.h"

namespace spring {

namespace {

std::string read_order_file_path(const std::string &temp_dir) {
  return temp_dir + "/read_order.bin";
}

void load_reordered_positions(const std::string &read_order_path,
                              std::vector<uint32_t> &reordered_positions,
                              std::vector<uint32_t> &read_index_by_order) {
  std::ifstream order_input(read_order_path, std::ios::binary);
  uint32_t current_order;
  for (uint32_t read_index = 0; read_index < reordered_positions.size();
       read_index++) {
    order_input.read(byte_ptr(&current_order), sizeof(uint32_t));
    reordered_positions[read_index] = current_order;
    read_index_by_order[current_order] = read_index;
  }
}

void reorder_first_mates(std::vector<uint32_t> &reordered_positions,
                         const uint32_t mate_count) {
  uint32_t next_first_mate_order = 0;
  for (uint32_t read_index = 0; read_index < reordered_positions.size();
       read_index++) {
    if (reordered_positions[read_index] < mate_count)
      reordered_positions[read_index] = next_first_mate_order++;
  }
}

void reorder_second_mates(std::vector<uint32_t> &reordered_positions,
                          const std::vector<uint32_t> &read_index_by_order,
                          const uint32_t mate_count) {
  for (uint32_t read_index = 0; read_index < reordered_positions.size();
       read_index++) {
    if (reordered_positions[read_index] < mate_count)
      continue;

    const uint32_t original_order = reordered_positions[read_index];
    const uint32_t mate_original_order = original_order - mate_count;
    const uint32_t mate_read_index = read_index_by_order[mate_original_order];
    const uint32_t mate_reordered_position =
        reordered_positions[mate_read_index];
    reordered_positions[read_index] = mate_reordered_position + mate_count;
  }
}

void persist_reordered_positions(
    const std::string &read_order_path,
    const std::vector<uint32_t> &reordered_positions) {
  const std::string temporary_output_path = read_order_path + ".tmp";
  std::ofstream order_output(temporary_output_path, std::ios::binary);
  for (uint32_t read_index = 0; read_index < reordered_positions.size();
       read_index++) {
    order_output.write(byte_ptr(&reordered_positions[read_index]),
                       sizeof(uint32_t));
  }
  order_output.close();
  safe_remove_file(read_order_path);
  safe_rename_file(temporary_output_path, read_order_path);
}

} // namespace

void pe_encode(const std::string &temp_dir, const compression_params &cp) {
  const uint32_t num_reads = cp.read_info.num_reads;
  const uint32_t mate_count = num_reads / 2;
  const std::string read_order_path = read_order_file_path(temp_dir);
  std::vector<uint32_t> reordered_positions(num_reads);
  std::vector<uint32_t> read_index_by_order(num_reads);

  load_reordered_positions(read_order_path, reordered_positions,
                           read_index_by_order);

  // File 1 keeps its reordered traversal; file 2 follows its mate positions.
  reorder_first_mates(reordered_positions, mate_count);
  reorder_second_mates(reordered_positions, read_index_by_order, mate_count);

  persist_reordered_positions(read_order_path, reordered_positions);
}

} // namespace spring
