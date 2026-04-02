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
