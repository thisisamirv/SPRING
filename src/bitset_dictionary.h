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

#ifndef SPRING_BITSET_DICTIONARY_H_
#define SPRING_BITSET_DICTIONARY_H_

#include "BooPHF.h"
#include "util.h"
#include <algorithm>
#include <bitset>
#include <cstdint>
#include <fstream>
#include <omp.h>
#include <string>

namespace spring {

using hasher_t = boomphf::SingleHashFunctor<uint64_t>;
using boophf_t = boomphf::mphf<uint64_t, hasher_t>;

inline std::string keys_bin_path(const std::string &base_dir,
                                 const int thread_id) {
  return base_dir + "/keys.bin." + std::to_string(thread_id);
}

inline std::string hash_bin_path(const std::string &base_dir,
                                 const int thread_id,
                                 const int dict_index) {
  return base_dir + "/hash.bin." + std::to_string(thread_id) + '.' +
         std::to_string(dict_index);
}

class bbhashdict {
public:
  boophf_t *bphf;
  int start;
  int end;
  uint32_t numkeys;
  uint32_t dict_numreads; // Number of reads long enough to participate here.
  uint32_t *startpos;
  uint32_t *read_id;
  bool *empty_bin;
  void findpos(int64_t *dictidx, const uint64_t &startposidx);
  void remove(const int64_t *dictidx, const uint64_t &startposidx,
              const int64_t current);
  bbhashdict()
      : bphf(NULL), start(0), end(0), numkeys(0), dict_numreads(0),
        startpos(NULL), read_id(NULL), empty_bin(NULL) {}
  bbhashdict(const bbhashdict &) = delete;
  bbhashdict &operator=(const bbhashdict &) = delete;
  ~bbhashdict() {
    if (startpos != NULL)
      delete[] startpos;
    if (read_id != NULL)
      delete[] read_id;
    if (empty_bin != NULL)
      delete[] empty_bin;
    if (bphf != NULL)
      delete bphf;
  }
};

template <size_t bitset_size>
void stringtobitset(const std::string &s, const uint16_t readlen,
                    std::bitset<bitset_size> &b,
                    std::bitset<bitset_size> **basemask) {
  for (int i = 0; i < readlen; i++)
    b |= basemask[i][(uint8_t)s[i]];
}

template <size_t bitset_size>
void generateindexmasks(std::bitset<bitset_size> *index_masks,
                        bbhashdict *dict,
                        int numdict, int bpb) {
  for (int j = 0; j < numdict; j++)
    index_masks[j].reset();
  for (int j = 0; j < numdict; j++)
    for (int i = bpb * dict[j].start; i < bpb * (dict[j].end + 1); i++)
      index_masks[j][i] = 1;
  return;
}

template <size_t bitset_size>
void constructdictionary(std::bitset<bitset_size> *read, bbhashdict *dict,
                         uint16_t *read_lengths, const int numdict,
                         const uint32_t &numreads, const int bpb,
                         const std::string &basedir, const int &num_thr) {
  std::bitset<bitset_size> *index_masks =
      new std::bitset<bitset_size>[numdict];
  generateindexmasks<bitset_size>(index_masks, dict, numdict, bpb);
  for (int j = 0; j < numdict; j++) {
    bbhashdict &current_dict = dict[j];
    uint64_t *dictionary_keys = new uint64_t[numreads];
#pragma omp parallel
    {
      std::bitset<bitset_size> masked_read_bits;
      const int thread_id = omp_get_thread_num();
      const int thread_count = omp_get_num_threads();
      uint64_t i, stop;
      i = uint64_t(thread_id) * numreads / thread_count;
      stop = uint64_t(thread_id + 1) * numreads / thread_count;
      if (thread_id == thread_count - 1)
        stop = numreads;
      // Compute this dictionary's masked keys for the thread's slice.
      for (; i < stop; i++) {
        masked_read_bits = read[i] & index_masks[j];
        dictionary_keys[i] =
            (masked_read_bits >> bpb * current_dict.start).to_ullong();
      }
    }

    // Discard reads that are shorter than the dictionary's end position.
    current_dict.dict_numreads = 0;
    for (uint32_t i = 0; i < numreads; i++) {
      if (read_lengths[i] > current_dict.end) {
        dictionary_keys[current_dict.dict_numreads] = dictionary_keys[i];
        current_dict.dict_numreads++;
      }
    }

    // Materialize per-thread key chunks so later passes can stream them again.
#pragma omp parallel
    {
      const int thread_id = omp_get_thread_num();
      const int thread_count = omp_get_num_threads();
      std::ofstream key_output(keys_bin_path(basedir, thread_id),
                              std::ios::binary);
      uint64_t i, stop;
      i = uint64_t(thread_id) * current_dict.dict_numreads / thread_count;
      stop =
          uint64_t(thread_id + 1) * current_dict.dict_numreads / thread_count;
      if (thread_id == thread_count - 1)
        stop = current_dict.dict_numreads;
      for (; i < stop; i++)
        key_output.write(byte_ptr(&dictionary_keys[i]), sizeof(uint64_t));
      key_output.close();
    }

    // Deduplicate the sorted key list before building the MPHF.
    std::sort(dictionary_keys, dictionary_keys + current_dict.dict_numreads);
    uint32_t unique_key_index = 0;
    for (uint32_t i = 1; i < current_dict.dict_numreads; i++)
      if (dictionary_keys[i] != dictionary_keys[unique_key_index])
        dictionary_keys[++unique_key_index] = dictionary_keys[i];
    current_dict.numkeys = unique_key_index + 1;

    // Build the MPHF over the distinct keys.
    auto data_iterator =
        boomphf::range(static_cast<const uint64_t *>(dictionary_keys),
                       static_cast<const uint64_t *>(dictionary_keys +
                                                    current_dict.numkeys));
    const double gamma_factor = 5.0; // balance between speed and memory
    current_dict.bphf = new boomphf::mphf<uint64_t, hasher_t>(
        current_dict.numkeys, data_iterator, num_thr, gamma_factor, true,
        false);

    delete[] dictionary_keys;

    // Re-read the stored keys and materialize their hash buckets.
#pragma omp parallel
    {
      const int thread_id = omp_get_thread_num();
      const int thread_count = omp_get_num_threads();
      const std::string key_path = keys_bin_path(basedir, thread_id);
      const std::string hash_path = hash_bin_path(basedir, thread_id, j);
      std::ifstream key_input(key_path, std::ios::binary);
      std::ofstream hash_output(hash_path, std::ios::binary);
      uint64_t current_key;
      uint64_t current_hash;
      uint64_t i, stop;
      i = uint64_t(thread_id) * current_dict.dict_numreads / thread_count;
      stop =
          uint64_t(thread_id + 1) * current_dict.dict_numreads / thread_count;
      if (thread_id == thread_count - 1)
        stop = current_dict.dict_numreads;
      for (; i < stop; i++) {
        key_input.read(byte_ptr(&current_key), sizeof(uint64_t));
        current_hash = current_dict.bphf->lookup(current_key);
        hash_output.write(byte_ptr(&current_hash), sizeof(uint64_t));
      }
      key_input.close();
      remove(key_path.c_str());
      hash_output.close();
    }
  }

  // The remaining passes parallelize across dictionaries instead of reads.
  omp_set_num_threads(std::min(numdict, num_thr));
#pragma omp parallel
  {
#pragma omp for
    for (int j = 0; j < numdict; j++) {
      bbhashdict &current_dict = dict[j];
      // Count bucket sizes, then prefix-sum them into start positions.
      current_dict.startpos =
          new uint32_t[current_dict.numkeys +
                       1](); // 1 extra to store end pos of last key
      uint64_t current_hash;
      for (int thread_id = 0; thread_id < num_thr; thread_id++) {
        std::ifstream hash_input(hash_bin_path(basedir, thread_id, j),
                                 std::ios::binary);
        hash_input.read(byte_ptr(&current_hash), sizeof(uint64_t));
        while (!hash_input.eof()) {
          current_dict.startpos[current_hash + 1]++;
          hash_input.read(byte_ptr(&current_hash), sizeof(uint64_t));
        }
        hash_input.close();
      }

      current_dict.empty_bin = new bool[current_dict.numkeys]();
      for (uint32_t i = 1; i < current_dict.numkeys; i++)
        current_dict.startpos[i] =
            current_dict.startpos[i] + current_dict.startpos[i - 1];

      // Populate the per-key read lists using the streamed hash files.
      current_dict.read_id = new uint32_t[current_dict.dict_numreads];
      uint32_t i = 0;
      for (int thread_id = 0; thread_id < num_thr; thread_id++) {
        const std::string hash_path = hash_bin_path(basedir, thread_id, j);
        std::ifstream hash_input(hash_path, std::ios::binary);
        hash_input.read(byte_ptr(&current_hash), sizeof(uint64_t));
        while (!hash_input.eof()) {
          while (read_lengths[i] <= current_dict.end)
            i++;
          current_dict.read_id[current_dict.startpos[current_hash]++] = i;
          i++;
          hash_input.read(byte_ptr(&current_hash), sizeof(uint64_t));
        }
        hash_input.close();
        remove(hash_path.c_str());
      }

      // Restore the prefix offsets after the insertion walk above.
      for (int64_t keynum = current_dict.numkeys; keynum >= 1; keynum--)
        current_dict.startpos[keynum] = current_dict.startpos[keynum - 1];
      current_dict.startpos[0] = 0;
    }
  }
  omp_set_num_threads(num_thr);
  delete[] index_masks;
  return;
}

template <size_t bitset_size>
void generatemasks(std::bitset<bitset_size> **mask, const int max_readlen,
                   const int bpb) {
  // Zero trailing positions so shifted reads can be compared consistently.
  for (int i = 0; i < max_readlen; i++) {
    for (int j = 0; j < max_readlen; j++) {
      mask[i][j].reset();
      for (int k = bpb * i; k < bpb * max_readlen - bpb * j; k++)
        mask[i][j][k] = 1;
    }
  }
  return;
}

template <size_t bitset_size>
void chartobitset(char *s, const int readlen, std::bitset<bitset_size> &b,
                  std::bitset<bitset_size> **basemask) {
  b.reset();
  for (int i = 0; i < readlen; i++)
    b |= basemask[i][(uint8_t)s[i]];
  return;
}

} // namespace spring

#endif // SPRING_BITSET_DICTIONARY_H_
