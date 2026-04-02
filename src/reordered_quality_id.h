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

// Declares the quality/id reordering and compression stage that follows read
// reordering during archive construction.

#ifndef SPRING_REORDERED_QUALITY_ID_H_
#define SPRING_REORDERED_QUALITY_ID_H_

#include <string>

namespace spring {

struct compression_params;

// Reorder preserved ids and qualities to match the post-reorder read layout.
void reorder_compress_quality_id(const std::string &temp_dir,
                                 const compression_params &cp);

} // namespace spring

#endif // SPRING_REORDERED_QUALITY_ID_H_
