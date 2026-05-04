// Reorders paired-end mate indices to match Spring's reordered read layout
// while preserving pair relationships for later reconstruction.

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "paired_end_mate_ordering.h"
#include "params.h"
#include "progress.h"

namespace spring {

namespace {

void validate_reordered_positions(
    const std::vector<uint32_t> &reordered_positions,
    const std::string &block_id, std::vector<uint32_t> &read_index_by_order) {
  for (uint32_t read_index = 0; read_index < reordered_positions.size();
       read_index++) {
    const uint32_t current_order = reordered_positions[read_index];
    if (current_order >= reordered_positions.size()) {
      SPRING_LOG_DEBUG(
          "block_id=" + block_id +
          ", pe_encode read-order out-of-range value: expected_bytes=" +
          std::to_string(reordered_positions.size()) +
          ", actual_bytes=" + std::to_string(current_order) +
          ", index=" + std::to_string(read_index));
      throw std::runtime_error("Paired-end order value out of range.");
    }
    if (read_index_by_order[current_order] != UINT32_MAX) {
      SPRING_LOG_DEBUG(
          "block_id=" + block_id +
          ", pe_encode read-order duplicate value: expected_bytes=" +
          std::to_string(reordered_positions.size()) +
          ", actual_bytes=" + std::to_string(current_order) +
          ", index=" + std::to_string(read_index));
      throw std::runtime_error("Paired-end order contains duplicates.");
    }
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

} // namespace

void pe_encode(std::vector<uint32_t> &read_order_entries,
               const compression_params &cp) {
  const std::string block_id = "pe-order-main";
  const uint32_t num_reads = cp.read_info.num_reads;
  if (num_reads % 2 != 0) {
    SPRING_LOG_DEBUG(
        "block_id=" + block_id + ", pe_encode invalid read count:" +
        ", expected_bytes=0, actual_bytes=" + std::to_string(num_reads) +
        ", index=0");
    throw std::runtime_error(
        "Paired-end mode requires an even number of reads.");
  }
  if (read_order_entries.size() != num_reads) {
    SPRING_LOG_DEBUG("block_id=" + block_id +
                     ", pe_encode invalid order entry count: expected_bytes=" +
                     std::to_string(num_reads) + ", actual_bytes=" +
                     std::to_string(read_order_entries.size()) + ", index=0");
    throw std::runtime_error("Paired-end order is not a full permutation.");
  }
  const uint32_t mate_count = num_reads / 2;
  std::vector<uint32_t> read_index_by_order(num_reads, UINT32_MAX);
  validate_reordered_positions(read_order_entries, block_id,
                               read_index_by_order);

  for (uint32_t order = 0; order < num_reads; order++) {
    if (read_index_by_order[order] == UINT32_MAX) {
      SPRING_LOG_DEBUG("block_id=" + block_id +
                       ", pe_encode missing order entry: expected_bytes=" +
                       std::to_string(num_reads) +
                       ", actual_bytes=" + std::to_string(order) +
                       ", index=" + std::to_string(order));
      throw std::runtime_error("Paired-end order is not a full permutation.");
    }
  }

  // File 1 keeps its reordered traversal; file 2 follows its mate positions.
  reorder_first_mates(read_order_entries, mate_count);
  reorder_second_mates(read_order_entries, read_index_by_order, mate_count);
}

} // namespace spring
