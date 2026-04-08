// Declares the short-read, long-read, and packed-sequence decompression entry
// points used by Spring's top-level archive restoration flow.

#ifndef SPRING_DECOMPRESS_H_
#define SPRING_DECOMPRESS_H_

#include <string>

namespace spring {

struct compression_params;

// Short-read archives reconstruct aligned and unaligned records separately.
void decompress_short(const std::string &temp_dir, const std::string &outfile_1,
                      const std::string &outfile_2, compression_params &cp,
                      const bool use_crlf);

// Long-read archives store read streams directly, without reference-based
// reconstruction.
void decompress_long(const std::string &temp_dir, const std::string &outfile_1,
                     const std::string &outfile_2, compression_params &cp,
                     const bool use_crlf);

// Packed reference chunks are decoded once, then concatenated by callers.
void decompress_unpack_seq(const std::string &packed_seq_base_path,
                           int encoding_thread_count, int decoding_thread_count,
                           const compression_params &cp);

} // namespace spring

#endif // SPRING_DECOMPRESS_H_
