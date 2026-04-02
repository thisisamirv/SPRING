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

// Builds and updates the BBHash-backed read dictionaries used by Spring's
// reorder and encode stages for candidate lookup by packed read keys.

#include "bitset_dictionary.h"
#include "params.h"
#include <cstdint>

namespace spring {

namespace {

uint32_t bucket_tail_marker(const uint32_t *read_ids,
                            const uint32_t bucket_end_index) {
  return read_ids[bucket_end_index - 1];
}

int64_t logical_bin_end(const uint32_t *read_ids, const int64_t bin_begin,
                        const uint32_t bucket_end_index) {
  const uint32_t tail_marker =
      bucket_tail_marker(read_ids, bucket_end_index);

  if (tail_marker == MAX_NUM_READS)
    return bucket_end_index - 1;

  if (tail_marker == MAX_NUM_READS + 1)
    return bin_begin + read_ids[bucket_end_index - 2];

  return bucket_end_index;
}

void update_tail_marker_after_remove(uint32_t *read_ids,
                                     const int64_t logical_end,
                                     const int64_t original_bin_size,
                                     const uint32_t bucket_end_index) {
  const uint32_t tail_marker =
      bucket_tail_marker(read_ids, bucket_end_index);

  if (logical_end == bucket_end_index) {
    read_ids[bucket_end_index - 1] = MAX_NUM_READS;
    return;
  }

  if (tail_marker == MAX_NUM_READS) {
    read_ids[bucket_end_index - 1] = MAX_NUM_READS + 1;
    read_ids[bucket_end_index - 2] =
        static_cast<uint32_t>(original_bin_size - 1);
    return;
  }

  read_ids[bucket_end_index - 2]--;
}

} // namespace

void bbhashdict::findpos(int64_t *dictidx, const uint64_t &startposidx) {
  const int64_t bin_begin = startpos[startposidx];
  const uint32_t bucket_end_index = startpos[startposidx + 1];

  dictidx[0] = bin_begin;
  dictidx[1] = logical_bin_end(read_id, bin_begin, bucket_end_index);
}

void bbhashdict::remove(const int64_t *dictidx, const uint64_t &startposidx,
                        const int64_t read_id_to_remove) {
  const int64_t bin_begin = dictidx[0];
  const int64_t logical_end = dictidx[1];
  const int64_t bin_size = logical_end - bin_begin;

  if (bin_size == 1) {
    empty_bin[startposidx] = true;
    return;
  }

  const int64_t remove_offset =
      std::lower_bound(read_id + bin_begin, read_id + logical_end,
                       read_id_to_remove) -
      (read_id + bin_begin);

  for (int64_t index = bin_begin + remove_offset; index < logical_end - 1;
       index++) {
    read_id[index] = read_id[index + 1];
  }

  const uint32_t bucket_end_index = startpos[startposidx + 1];
  update_tail_marker_after_remove(read_id, logical_end, bin_size,
                                  bucket_end_index);
}

} // namespace spring
