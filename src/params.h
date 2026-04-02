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
constexpr int NUM_LOCKS_REORDER = 0x1000000;
constexpr float STOP_CRITERIA_REORDER = 0.5F;

// Encoding parameters.
constexpr int NUM_DICT_ENCODER = 2;
constexpr int MAX_SEARCH_ENCODER = 1000;
constexpr int THRESH_ENCODER = 24;

// Block sizing parameters for stream chunking and BSC compression.
constexpr int NUM_READS_PER_BLOCK = 256000;
constexpr int NUM_READS_PER_BLOCK_LONG = 10000;
constexpr int BSC_BLOCK_SIZE = 64;
} // namespace spring

#endif // SPRING_PARAMS_H_
