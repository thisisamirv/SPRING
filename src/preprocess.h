// Declares the preprocessing stage that converts raw input reads into the
// temporary files consumed by Spring's reorder and encode passes.

#ifndef SPRING_PREPROCESS_H_
#define SPRING_PREPROCESS_H_

#include <string>

namespace spring {

struct compression_params;

// Normalize input reads into Spring's temporary block files and side streams.
void preprocess(const std::string &infile_1, const std::string &infile_2,
                const std::string &temp_dir, compression_params &cp,
                const bool &gzip_flag, const bool &fasta_flag);

} // namespace spring

#endif // SPRING_PREPROCESS_H_
