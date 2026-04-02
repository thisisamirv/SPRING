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

inline std::string keys_bin_path(const std::string &basedir, const int tid) {
  return basedir + "/keys.bin." + std::to_string(tid);
}

inline std::string hash_bin_path(const std::string &basedir, const int tid,
                                 const int dict_index) {
  return basedir + "/hash.bin." + std::to_string(tid) + '.' +
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
void generateindexmasks(std::bitset<bitset_size> *mask1, bbhashdict *dict,
                        int numdict, int bpb) {
  for (int j = 0; j < numdict; j++)
    mask1[j].reset();
  for (int j = 0; j < numdict; j++)
    for (int i = bpb * dict[j].start; i < bpb * (dict[j].end + 1); i++)
      mask1[j][i] = 1;
  return;
}

template <size_t bitset_size>
void constructdictionary(std::bitset<bitset_size> *read, bbhashdict *dict,
                         uint16_t *read_lengths, const int numdict,
                         const uint32_t &numreads, const int bpb,
                         const std::string &basedir, const int &num_thr) {
  std::bitset<bitset_size> *mask = new std::bitset<bitset_size>[numdict];
  generateindexmasks<bitset_size>(mask, dict, numdict, bpb);
  for (int j = 0; j < numdict; j++) {
    bbhashdict &current_dict = dict[j];
    uint64_t *ull = new uint64_t[numreads];
#pragma omp parallel
    {
      std::bitset<bitset_size> b;
      const int tid = omp_get_thread_num();
      const int thread_count = omp_get_num_threads();
      uint64_t i, stop;
      i = uint64_t(tid) * numreads / thread_count;
      stop = uint64_t(tid + 1) * numreads / thread_count;
      if (tid == thread_count - 1)
        stop = numreads;
      // Compute this dictionary's masked keys for the thread's slice.
      for (; i < stop; i++) {
        b = read[i] & mask[j];
        ull[i] = (b >> bpb * current_dict.start).to_ullong();
      }
    }

    // Discard reads that are shorter than the dictionary's end position.
    current_dict.dict_numreads = 0;
    for (uint32_t i = 0; i < numreads; i++) {
      if (read_lengths[i] > current_dict.end) {
        ull[current_dict.dict_numreads] = ull[i];
        current_dict.dict_numreads++;
      }
    }

  // Materialize per-thread key chunks so later passes can stream them again.
#pragma omp parallel
    {
      const int tid = omp_get_thread_num();
      const int thread_count = omp_get_num_threads();
      std::ofstream foutkey(keys_bin_path(basedir, tid), std::ios::binary);
      uint64_t i, stop;
      i = uint64_t(tid) * current_dict.dict_numreads / thread_count;
      stop = uint64_t(tid + 1) * current_dict.dict_numreads / thread_count;
      if (tid == thread_count - 1)
        stop = current_dict.dict_numreads;
      for (; i < stop; i++)
        foutkey.write(byte_ptr(&ull[i]), sizeof(uint64_t));
      foutkey.close();
    }

    // Deduplicate the sorted key list before building the MPHF.
    std::sort(ull, ull + current_dict.dict_numreads);
    uint32_t k = 0;
    for (uint32_t i = 1; i < current_dict.dict_numreads; i++)
      if (ull[i] != ull[k])
        ull[++k] = ull[i];
    current_dict.numkeys = k + 1;

    // Build the MPHF over the distinct keys.
    auto data_iterator =
        boomphf::range(static_cast<const uint64_t *>(ull),
                       static_cast<const uint64_t *>(ull + current_dict.numkeys));
    const double gamma_factor = 5.0; // balance between speed and memory
    current_dict.bphf = new boomphf::mphf<uint64_t, hasher_t>(
        current_dict.numkeys, data_iterator, num_thr, gamma_factor, true,
        false);

    delete[] ull;

  // Re-read the stored keys and materialize their hash buckets.
#pragma omp parallel
    {
      const int tid = omp_get_thread_num();
      const int thread_count = omp_get_num_threads();
      const std::string key_path = keys_bin_path(basedir, tid);
      const std::string hash_path = hash_bin_path(basedir, tid, j);
      std::ifstream finkey(key_path, std::ios::binary);
      std::ofstream fouthash(hash_path, std::ios::binary);
      uint64_t currentkey, currenthash;
      uint64_t i, stop;
      i = uint64_t(tid) * current_dict.dict_numreads / thread_count;
      stop = uint64_t(tid + 1) * current_dict.dict_numreads / thread_count;
      if (tid == thread_count - 1)
        stop = current_dict.dict_numreads;
      for (; i < stop; i++) {
        finkey.read(byte_ptr(&currentkey), sizeof(uint64_t));
        currenthash = current_dict.bphf->lookup(currentkey);
        fouthash.write(byte_ptr(&currenthash), sizeof(uint64_t));
      }
      finkey.close();
      remove(key_path.c_str());
      fouthash.close();
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
      uint64_t currenthash;
      for (int tid = 0; tid < num_thr; tid++) {
        std::ifstream finhash(hash_bin_path(basedir, tid, j), std::ios::binary);
        finhash.read(byte_ptr(&currenthash), sizeof(uint64_t));
        while (!finhash.eof()) {
          current_dict.startpos[currenthash + 1]++;
          finhash.read(byte_ptr(&currenthash), sizeof(uint64_t));
        }
        finhash.close();
      }

      current_dict.empty_bin = new bool[current_dict.numkeys]();
      for (uint32_t i = 1; i < current_dict.numkeys; i++)
        current_dict.startpos[i] =
            current_dict.startpos[i] + current_dict.startpos[i - 1];

      // Populate the per-key read lists using the streamed hash files.
      current_dict.read_id = new uint32_t[current_dict.dict_numreads];
      uint32_t i = 0;
      for (int tid = 0; tid < num_thr; tid++) {
        const std::string hash_path = hash_bin_path(basedir, tid, j);
        std::ifstream finhash(hash_path, std::ios::binary);
        finhash.read(byte_ptr(&currenthash), sizeof(uint64_t));
        while (!finhash.eof()) {
          while (read_lengths[i] <= current_dict.end)
            i++;
          current_dict.read_id[current_dict.startpos[currenthash]++] = i;
          i++;
          finhash.read(byte_ptr(&currenthash), sizeof(uint64_t));
        }
        finhash.close();
        remove(hash_path.c_str());
      }

      // Restore the prefix offsets after the insertion walk above.
      for (int64_t keynum = current_dict.numkeys; keynum >= 1; keynum--)
        current_dict.startpos[keynum] = current_dict.startpos[keynum - 1];
      current_dict.startpos[0] = 0;
    }
  }
  omp_set_num_threads(num_thr);
  delete[] mask;
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
