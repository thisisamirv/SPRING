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

#include "bitset_util.h"
#include "params.h"
#include <cstdint>

namespace spring {

void bbhashdict::findpos(int64_t *dictidx, const uint64_t &startposidx) {
  dictidx[0] = startpos[startposidx];
  const uint32_t endidx = startpos[startposidx + 1];
  // Tail markers record whether deletions have compacted the logical bin end.
  const uint32_t tail_marker = read_id[endidx - 1];

  if (tail_marker == MAX_NUM_READS) {
    dictidx[1] = endidx - 1;
    return;
  }

  if (tail_marker == MAX_NUM_READS + 1) {
    dictidx[1] = dictidx[0] + read_id[endidx - 2];
    return;
  }

  dictidx[1] = endidx;
}

void bbhashdict::remove(const int64_t *dictidx, const uint64_t &startposidx,
                        const int64_t current) {
  const int64_t bin_begin = dictidx[0];
  const int64_t bin_end = dictidx[1];
  const int64_t bin_size = bin_end - bin_begin;

  if (bin_size == 1) {
    empty_bin[startposidx] = true;
    return;
  }

  const int64_t pos =
      std::lower_bound(read_id + bin_begin, read_id + bin_end, current) -
      (read_id + bin_begin);

  for (int64_t i = bin_begin + pos; i < bin_end - 1; i++) {
    read_id[i] = read_id[i + 1];
  }

  const uint32_t endidx = startpos[startposidx + 1];
  const uint32_t tail_marker = read_id[endidx - 1];

  if (bin_end == endidx) {
    read_id[endidx - 1] = MAX_NUM_READS;
    return;
  }

  if (tail_marker == MAX_NUM_READS) {
    read_id[endidx - 1] = MAX_NUM_READS + 1;
    read_id[endidx - 2] =
        static_cast<uint32_t>(bin_size - 1);
    return;
  }

  read_id[endidx - 2]--;
}

} // namespace spring
