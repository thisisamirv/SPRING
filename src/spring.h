/*
* Copyright 2018 University of Illinois Board of Trustees and Stanford
University. All Rights Reserved.
* Licensed under the “Non-exclusive Research Use License for SPRING Software”
license (the "License");
* You may not use this file except in compliance with the License.
* The License is included in the distribution as license.pdf file.

* Software distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
limitations under the License.
*/

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
                            const string_list &infile_vec, const string_list &outfile_vec,
                            const int &num_thr,
              const bool &pairing_only_flag, const bool &no_quality_flag,
                            const bool &no_ids_flag, const string_list &quality_opts,
              const bool &long_flag, const bool &gzip_flag,
              const bool &fasta_flag);

void decompress(const std::string &temp_dir,
                                const string_list &infile_vec, const string_list &outfile_vec,
                                const int &num_thr, const read_range &decompress_range_vec,
                const bool &gzip_flag, const int &gzip_level);

std::string random_string(size_t length);

} // namespace spring

#endif // SPRING_SPRING_H_
