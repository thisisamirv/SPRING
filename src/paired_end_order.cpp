// Reorders paired-end mate indices to match Spring's reordered read layout
// while preserving pair relationships for later reconstruction.

#include <fstream>
#include <iostream>
#include <filesystem>
#include <vector>

#include "core_utils.h"
#include "fs_utils.h"
#include "paired_end_order.h"
#include "params.h"
#include "progress.h"

namespace spring {

namespace {

std::string read_order_file_path(const std::string &temp_dir) {
  return temp_dir + "/read_order.bin";
}

void load_reordered_positions(const std::string &read_order_path,
                              const std::string &block_id,
                              std::vector<uint32_t> &reordered_positions,
                              std::vector<uint32_t> &read_index_by_order) {
  std::ifstream order_input(read_order_path, std::ios::binary);
  if (!order_input.is_open()) {
    Logger::log_debug("block_id=" + block_id +
                      ", pe_encode read-order open failure: path=" +
                      read_order_path +
                      ", expected_bytes=1, actual_bytes=0, index=0");
    throw std::runtime_error("Failed to open paired-end read order file.");
  }

  std::error_code file_ec;
  const uint64_t actual_bytes = std::filesystem::file_size(read_order_path, file_ec);
  const uint64_t expected_bytes =
      static_cast<uint64_t>(reordered_positions.size()) * sizeof(uint32_t);
  if (file_ec || actual_bytes < expected_bytes) {
    Logger::log_debug("block_id=" + block_id +
              ", pe_encode read-order size mismatch: path=" +
                      read_order_path +
                      ", expected_bytes=" + std::to_string(expected_bytes) +
                      ", actual_bytes=" + std::to_string(actual_bytes));
    throw std::runtime_error("Paired-end read order file is truncated.");
  }

  uint32_t current_order;
  for (uint32_t read_index = 0; read_index < reordered_positions.size();
       read_index++) {
    if (!order_input.read(byte_ptr(&current_order), sizeof(uint32_t))) {
      Logger::log_debug("block_id=" + block_id +
                        ", pe_encode read-order short read: path=" +
                        read_order_path + ", expected_bytes=" +
                        std::to_string(sizeof(uint32_t)) + ", actual_bytes=" +
                        std::to_string(order_input.gcount()) +
                        ", index=" + std::to_string(read_index));
      throw std::runtime_error("Failed to read paired-end order record.");
    }
    if (current_order >= reordered_positions.size()) {
      Logger::log_debug("block_id=" + block_id +
                        ", pe_encode read-order out-of-range value: path=" +
                        read_order_path +
                        ", expected_bytes=" +
                        std::to_string(reordered_positions.size()) +
                        ", actual_bytes=" + std::to_string(current_order) +
                        ", index=" + std::to_string(read_index));
      throw std::runtime_error("Paired-end order value out of range.");
    }
    if (read_index_by_order[current_order] != UINT32_MAX) {
      Logger::log_debug("block_id=" + block_id +
                        ", pe_encode read-order duplicate value: path=" +
                        read_order_path + ", expected_bytes=" +
                        std::to_string(reordered_positions.size()) +
                        ", actual_bytes=" + std::to_string(current_order) +
                        ", index=" + std::to_string(read_index));
      throw std::runtime_error("Paired-end order contains duplicates.");
    }
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
    const std::string &read_order_path, const std::string &block_id,
    const std::vector<uint32_t> &reordered_positions) {
  const std::string temporary_output_path = read_order_path + ".tmp";
  std::ofstream order_output(temporary_output_path, std::ios::binary);
  if (!order_output.is_open()) {
    Logger::log_debug("block_id=" + block_id +
                      ", pe_encode write open failure: path=" +
                      temporary_output_path +
                      ", expected_bytes=1, actual_bytes=0, index=0");
    throw std::runtime_error("Failed to open temporary paired-end order file.");
  }
  for (uint32_t read_index = 0; read_index < reordered_positions.size();
       read_index++) {
    order_output.write(byte_ptr(&reordered_positions[read_index]), sizeof(uint32_t));
    if (!order_output.good()) {
      Logger::log_debug("block_id=" + block_id +
                        ", pe_encode write failure: path=" + temporary_output_path +
                        ", expected_bytes=" +
                        std::to_string(sizeof(uint32_t)) +
                        ", actual_bytes=0, index=" +
                        std::to_string(read_index));
      throw std::runtime_error("Failed while writing paired-end order file.");
    }
  }
  order_output.close();
  std::error_code file_ec;
  const uint64_t actual_bytes =
      std::filesystem::file_size(temporary_output_path, file_ec);
  const uint64_t expected_bytes =
      static_cast<uint64_t>(reordered_positions.size()) * sizeof(uint32_t);
  if (file_ec || actual_bytes != expected_bytes) {
    Logger::log_debug("block_id=" + block_id +
              ", pe_encode write size mismatch: path=" +
              temporary_output_path +
                      ", expected_bytes=" + std::to_string(expected_bytes) +
                      ", actual_bytes=" + std::to_string(actual_bytes));
    throw std::runtime_error("Paired-end order write produced unexpected size.");
  }
  safe_remove_file(read_order_path);
  safe_rename_file(temporary_output_path, read_order_path);
}

} // namespace

void pe_encode(const std::string &temp_dir, const compression_params &cp) {
  const std::string block_id = "pe-order-main";
  const uint32_t num_reads = cp.read_info.num_reads;
  if (num_reads % 2 != 0) {
    Logger::log_debug("block_id=" + block_id +
                      ", pe_encode invalid read count:" +
                      ", expected_bytes=0, actual_bytes=" +
                      std::to_string(num_reads) + ", index=0");
    throw std::runtime_error("Paired-end mode requires an even number of reads.");
  }
  const uint32_t mate_count = num_reads / 2;
  const std::string read_order_path = read_order_file_path(temp_dir);
  std::vector<uint32_t> reordered_positions(num_reads);
  std::vector<uint32_t> read_index_by_order(num_reads, UINT32_MAX);

  load_reordered_positions(read_order_path, block_id, reordered_positions,
                           read_index_by_order);

  for (uint32_t order = 0; order < num_reads; order++) {
    if (read_index_by_order[order] == UINT32_MAX) {
      Logger::log_debug("block_id=" + block_id +
                        ", pe_encode missing order entry: path=" +
                        read_order_path + ", expected_bytes=" +
                        std::to_string(num_reads) + ", actual_bytes=" +
                        std::to_string(order) + ", index=" +
                        std::to_string(order));
      throw std::runtime_error("Paired-end order is not a full permutation.");
    }
  }

  // File 1 keeps its reordered traversal; file 2 follows its mate positions.
  reorder_first_mates(reordered_positions, mate_count);
  reorder_second_mates(reordered_positions, read_index_by_order, mate_count);

  persist_reordered_positions(read_order_path, block_id, reordered_positions);
}

} // namespace spring
