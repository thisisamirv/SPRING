// Declares the stream-reordering stage that materializes archive-ready
// position, noise, and unaligned data after reads have been reordered.

#ifndef SPRING_STREAM_REORDERING_H_
#define SPRING_STREAM_REORDERING_H_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace spring {

struct compression_params;

struct reordered_stream_artifact {
  std::vector<char> orientation_entries;
  std::vector<uint64_t> position_entries;
  std::vector<uint16_t> read_length_entries;
  std::vector<uint32_t> read_order_entries;
  std::vector<char> noise_serialized;
  std::vector<uint16_t> noise_positions;
  std::vector<char> unaligned_serialized;
  uint64_t unaligned_char_count = 0;
  std::unordered_map<std::string, std::string> archive_members;
};

// Rebuild the aligned and unaligned side streams into per-block archives.
std::unordered_map<std::string, std::string> reorder_compress_streams(
    const compression_params &cp, const reordered_stream_artifact &artifact,
    const std::vector<uint32_t> *read_order_override = nullptr);

} // namespace spring

#endif // SPRING_STREAM_REORDERING_H_
