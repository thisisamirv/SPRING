// Declares the preprocessing stage that converts raw input reads into the
// temporary files consumed by Spring's reorder and encode passes.

#ifndef SPRING_PREPROCESS_H_
#define SPRING_PREPROCESS_H_

#include <cstdint>
#include <string>

namespace spring {

struct compression_params;

// Normalize input reads into Spring's temporary block files and side streams.
void preprocess(const std::string &infile_1, const std::string &infile_2,
                const std::string &temp_dir, compression_params &cp,
                const bool &fasta_input);

// Quick pre-scan to determine the maximum read length across input files.
uint32_t detect_max_read_length(const std::string &infile_1,
                                const std::string &infile_2,
                                const bool paired_end, const bool fasta_input);

} // namespace spring

#endif // SPRING_PREPROCESS_H_
