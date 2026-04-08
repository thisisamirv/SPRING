// Centralizes compile-time tuning constants for read limits, dictionary search
// thresholds, and block sizes shared across the compression pipeline.

#ifndef SPRING_PARAMS_H_
#define SPRING_PARAMS_H_

#include <cstdint>

namespace spring {

// Shared bounds and sentinel values used across the compression pipeline.
constexpr uint16_t MAX_READ_LEN = 511;
constexpr uint32_t MAX_READ_LEN_LONG = 4294967290U;
constexpr uint32_t MAX_NUM_READS = 4294967290U;

// Reordering parameters.
constexpr int NUM_DICT_REORDER = 2;
constexpr int MAX_SEARCH_REORDER = 1000;
constexpr int THRESH_REORDER = 4;
// Keep this a power of two so lock sharding can use fast masking.
constexpr int NUM_LOCKS_REORDER = 0x10000;
constexpr float STOP_CRITERIA_REORDER = 0.5F;

namespace detail {
inline uint32_t lock_shard(const uint64_t item_id) {
  return static_cast<uint32_t>(item_id & (NUM_LOCKS_REORDER - 1));
}
} // namespace detail

// Encoding parameters.
constexpr int NUM_DICT_ENCODER = 2;
constexpr int MAX_SEARCH_ENCODER = 1000;
constexpr int THRESH_ENCODER = 24;

// Block sizing parameters for stream chunking and BSC compression.
constexpr int NUM_READS_PER_BLOCK = 256000;
constexpr int NUM_READS_PER_BLOCK_LONG = 10000;
constexpr int BSC_BLOCK_SIZE = 64;

// Default compression level (1-9) used by the CLI. This value is passed
// directly to gzip (1-9) and scaled to Zstd (1-22) where Zstd is used.
static constexpr int DEFAULT_COMPRESSION_LEVEL = 6;

// Maximum allowed growth (in bases) for a single consensus contig before
// forcing a break to prevent memory exhaustion or pathological reordering.
constexpr int64_t MAX_CONTIG_GROWTH = 64 * 1024 * 1024; // 64 MB
} // namespace spring

#endif // SPRING_PARAMS_H_
