// Declares the short-read, long-read, and packed-sequence decompression entry
// points used by Spring's top-level archive restoration flow.

#ifndef SPRING_DECOMPRESS_H_
#define SPRING_DECOMPRESS_H_

#include <cstdint>
#include <string>

namespace spring {

struct compression_params;

// Short-read archives reconstruct aligned and unaligned records separately.
void decompress_short(const std::string &temp_dir,
                      const std::string &output_path_1,
                      const std::string &output_path_2,
                      const compression_params &compression_params,
                      const int &num_threads,
                      const uint64_t &start_read_index,
                      const uint64_t &end_read_index,
                      const bool &gzip_enabled,
                      const int &gzip_level);

// Long-read archives store read streams directly, without reference-based
// reconstruction.
void decompress_long(const std::string &temp_dir,
                     const std::string &output_path_1,
                     const std::string &output_path_2,
                     const compression_params &compression_params,
                     const int &num_threads,
                     const uint64_t &start_read_index,
                     const uint64_t &end_read_index,
                     const bool &gzip_enabled,
                     const int &gzip_level);

// Packed reference chunks are decoded once, then concatenated by callers.
void decompress_unpack_seq(const std::string &packed_seq_base_path,
                           const int &encoding_thread_count,
                           const int &decoding_thread_count);

} // namespace spring

#endif // SPRING_DECOMPRESS_H_
