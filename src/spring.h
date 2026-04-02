// Declares Spring's CLI-facing compression and decompression entry points and
// the shared string/range aliases used to drive those workflows.

#ifndef SPRING_SPRING_H_
#define SPRING_SPRING_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace spring {

using string_list = std::vector<std::string>;
using read_range = std::vector<uint64_t>;

// Top-level compression and decompression entry points used by the CLI.
void compress(const std::string &temp_dir,
                            const string_list &input_paths,
                            const string_list &output_paths,
                            const int &num_thr,
              const bool &pairing_only_flag, const bool &no_quality_flag,
                            const bool &no_ids_flag,
                            const string_list &quality_options,
              const bool &long_flag, const bool &gzip_flag,
              const bool &fasta_flag);

void decompress(const std::string &temp_dir,
                                const string_list &input_paths,
                                const string_list &output_paths,
                                const int &num_thr,
                                const read_range &decompress_range,
                const bool &gzip_flag, const int &gzip_level);

std::string random_string(size_t length);

} // namespace spring

#endif // SPRING_SPRING_H_
