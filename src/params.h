// Centralizes compile-time tuning constants and runtime compression parameters.

#ifndef SPRING_PARAMS_H_
#define SPRING_PARAMS_H_

#include <cstdint>
#include <iosfwd>
#include <string>

namespace spring {

// Shared bounds and sentinel values used across the compression pipeline.
constexpr uint16_t MAX_READ_LEN = 511;
constexpr uint32_t MAX_READ_LEN_LONG = 4294967290U;
constexpr uint32_t MAX_NUM_READS = 4294967290U;

// Minimum poly-A/T run length (exclusive) required to strip the tail.
// Runs of exactly this length are kept; only strictly longer runs are stripped.
constexpr uint32_t POLY_AT_TAIL_MIN_LEN = 20;

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

// For small read pools, MPHF build overhead can dominate runtime. Use a
// single dictionary in reorder/encoder to reduce build cost with minimal
// sensitivity loss on tiny inputs.
constexpr uint32_t DICT_SINGLE_STAGE_READ_THRESHOLD = 50000;

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

struct compression_params {
  struct EncodingConfig {
    bool paired_end;
    bool preserve_order;
    bool preserve_quality;
    bool preserve_id;
    bool long_flag;
    int num_thr;
    int compression_level;
    int num_reads_per_block;
    int num_reads_per_block_long;
    bool fasta_mode;
    bool use_crlf;
    uint32_t cb_len = 16;      // CB length for extraction/display.
    bool barcode_sort = false; // Legacy field; always false in new archives.
    bool methyl_ternary = false;
    char depleted_base = 'N';
    bool poly_at_stripped = false;   // True when poly-A/T tail stripping was
                                     // applied during RNA-mode compression.
    bool cb_prefix_stripped = false; // True when CB prefix was extracted from
                                     // R1 and stored in cb_prefix.dna.bsc.
    uint32_t cb_prefix_len = 0;      // Number of bases stripped from R1 start.
  } encoding;

  struct QualityConfig {
    bool qvz_flag;
    double qvz_ratio;
    bool ill_bin_flag;
    bool bin_thr_flag;
    unsigned int bin_thr_thr;
    unsigned int bin_thr_high;
    unsigned int bin_thr_low;
  } quality;

  struct GzipMetadata {
    struct Stream {
      bool was_gzipped;
      uint8_t flg;
      uint32_t mtime;
      uint8_t xfl;
      uint8_t os;
      std::string name;
      bool is_bgzf;
      uint16_t bgzf_block_size;
      uint64_t uncompressed_size;
      uint64_t compressed_size;
      uint32_t member_count;
    } streams[2];
  } gzip;

  struct ReadMetadata {
    uint32_t num_reads;
    uint32_t num_reads_clean[2];
    uint32_t max_readlen;
    uint8_t paired_id_code;
    bool paired_id_match;
    static constexpr size_t kFileLenThrSize = 1024;
    uint64_t file_len_seq_thr[kFileLenThrSize];
    uint64_t file_len_id_thr[kFileLenThrSize];
    std::string input_filename_1;
    std::string input_filename_2;
    std::string note;
    std::string assay;
    std::string assay_confidence;
    uint32_t sequence_crc[2];
    uint32_t quality_crc[2];
    uint32_t id_crc[2];
  } read_info;
};

// Metadata serialization helpers.
void write_bool(std::ostream &out, bool value);
bool read_bool(std::istream &in);
void write_string(std::ostream &out, const std::string &s);
std::string read_string(std::istream &in);
void write_compression_params(std::ostream &out, const compression_params &cp);
void read_compression_params(std::istream &in, compression_params &cp);

} // namespace spring

#endif // SPRING_PARAMS_H_
