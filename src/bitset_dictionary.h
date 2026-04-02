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

// Declares the BBHash-based dictionary structures and construction helpers used
// to index packed reads during reordering and encoding.

#ifndef SPRING_BITSET_DICTIONARY_H_
#define SPRING_BITSET_DICTIONARY_H_

#include "BBHash/BooPHF.h"
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
              const int64_t read_id_to_remove);
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

namespace detail {

struct thread_range {
  uint64_t begin;
  uint64_t end;
};

inline thread_range split_thread_range(const uint64_t item_count,
                                       const int thread_id,
                                       const int thread_count) {
  thread_range range;
  range.begin = uint64_t(thread_id) * item_count / thread_count;
  range.end = uint64_t(thread_id + 1) * item_count / thread_count;
  if (thread_id == thread_count - 1)
    range.end = item_count;
  return range;
}

template <size_t bitset_size>
void compute_dictionary_keys(std::bitset<bitset_size> *read_bits,
                             const std::bitset<bitset_size> &index_mask,
                             const bbhashdict &dictionary,
                             const uint32_t read_count,
                             const int bits_per_base,
                             uint64_t *dictionary_keys) {
#pragma omp parallel
  {
    std::bitset<bitset_size> masked_read_bits;
    const int thread_id = omp_get_thread_num();
    const int thread_count = omp_get_num_threads();
    const thread_range range =
        split_thread_range(read_count, thread_id, thread_count);

    for (uint64_t read_index = range.begin; read_index < range.end;
         read_index++) {
      masked_read_bits = read_bits[read_index] & index_mask;
      dictionary_keys[read_index] =
          (masked_read_bits >> bits_per_base * dictionary.start).to_ullong();
    }
  }
}

inline uint32_t compact_dictionary_keys(uint64_t *dictionary_keys,
                                        const uint16_t *read_lengths,
                                        const uint32_t read_count,
                                        const int dictionary_end) {
  uint32_t eligible_read_count = 0;
  for (uint32_t read_index = 0; read_index < read_count; read_index++) {
    if (read_lengths[read_index] > dictionary_end) {
      dictionary_keys[eligible_read_count] = dictionary_keys[read_index];
      eligible_read_count++;
    }
  }

  return eligible_read_count;
}

inline void write_key_chunks(const uint64_t *dictionary_keys,
                             const uint32_t key_count,
                             const std::string &base_dir) {
#pragma omp parallel
  {
    const int thread_id = omp_get_thread_num();
    const int thread_count = omp_get_num_threads();
    const thread_range range = split_thread_range(key_count, thread_id,
                                                  thread_count);
    std::ofstream key_output(keys_bin_path(base_dir, thread_id),
                             std::ios::binary);

    for (uint64_t key_index = range.begin; key_index < range.end; key_index++)
      key_output.write(byte_ptr(&dictionary_keys[key_index]), sizeof(uint64_t));
  }
}

inline uint32_t sort_and_deduplicate_keys(uint64_t *dictionary_keys,
                                          const uint32_t key_count) {
  std::sort(dictionary_keys, dictionary_keys + key_count);

  uint32_t unique_key_index = 0;
  for (uint32_t key_index = 1; key_index < key_count; key_index++) {
    if (dictionary_keys[key_index] != dictionary_keys[unique_key_index])
      dictionary_keys[++unique_key_index] = dictionary_keys[key_index];
  }

  return unique_key_index + 1;
}

inline void write_hash_chunks(const bbhashdict &dictionary,
                              const std::string &base_dir,
                              const int dict_index) {
#pragma omp parallel
  {
    const int thread_id = omp_get_thread_num();
    const int thread_count = omp_get_num_threads();
    const thread_range range = split_thread_range(
        dictionary.dict_numreads, thread_id, thread_count);
    const std::string key_path = keys_bin_path(base_dir, thread_id);
    const std::string hash_path = hash_bin_path(base_dir, thread_id, dict_index);
    std::ifstream key_input(key_path, std::ios::binary);
    std::ofstream hash_output(hash_path, std::ios::binary);
    uint64_t current_key;

    for (uint64_t key_index = range.begin; key_index < range.end; key_index++) {
      key_input.read(byte_ptr(&current_key), sizeof(uint64_t));
      const uint64_t current_hash = dictionary.bphf->lookup(current_key);
      hash_output.write(byte_ptr(&current_hash), sizeof(uint64_t));
    }

    key_input.close();
    remove(key_path.c_str());
  }
}

inline void count_bucket_sizes(bbhashdict &dictionary,
                               const std::string &base_dir,
                               const int dict_index,
                               const int thread_count) {
  uint64_t current_hash;
  for (int thread_id = 0; thread_id < thread_count; thread_id++) {
    std::ifstream hash_input(hash_bin_path(base_dir, thread_id, dict_index),
                             std::ios::binary);
    hash_input.read(byte_ptr(&current_hash), sizeof(uint64_t));
    while (!hash_input.eof()) {
      dictionary.startpos[current_hash + 1]++;
      hash_input.read(byte_ptr(&current_hash), sizeof(uint64_t));
    }
  }
}

inline void finalize_bucket_offsets(bbhashdict &dictionary) {
  dictionary.empty_bin = new bool[dictionary.numkeys]();
  for (uint32_t key_index = 1; key_index < dictionary.numkeys; key_index++) {
    dictionary.startpos[key_index] += dictionary.startpos[key_index - 1];
  }
}

inline void populate_bucket_read_ids(bbhashdict &dictionary,
                                     uint16_t *read_lengths,
                                     const std::string &base_dir,
                                     const int dict_index,
                                     const int thread_count) {
  dictionary.read_id = new uint32_t[dictionary.dict_numreads];
  uint32_t read_index = 0;
  uint64_t current_hash;

  for (int thread_id = 0; thread_id < thread_count; thread_id++) {
    const std::string hash_path = hash_bin_path(base_dir, thread_id, dict_index);
    std::ifstream hash_input(hash_path, std::ios::binary);
    hash_input.read(byte_ptr(&current_hash), sizeof(uint64_t));
    while (!hash_input.eof()) {
      while (read_lengths[read_index] <= dictionary.end)
        read_index++;
      dictionary.read_id[dictionary.startpos[current_hash]++] = read_index;
      read_index++;
      hash_input.read(byte_ptr(&current_hash), sizeof(uint64_t));
    }
    remove(hash_path.c_str());
  }
}

inline void restore_bucket_starts(bbhashdict &dictionary) {
  for (int64_t key_index = dictionary.numkeys; key_index >= 1; key_index--)
    dictionary.startpos[key_index] = dictionary.startpos[key_index - 1];
  dictionary.startpos[0] = 0;
}

} // namespace detail

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

  for (int dict_index = 0; dict_index < numdict; dict_index++) {
    bbhashdict &current_dict = dict[dict_index];
    uint64_t *dictionary_keys = new uint64_t[numreads];

    detail::compute_dictionary_keys<bitset_size>(
        read, index_masks[dict_index], current_dict, numreads, bpb,
        dictionary_keys);

    // Keep only reads that are long enough for this dictionary.
    current_dict.dict_numreads = detail::compact_dictionary_keys(
        dictionary_keys, read_lengths, numreads, current_dict.end);

    // Persist keys because later passes stream them again while building
    // the bucket layout.
    detail::write_key_chunks(dictionary_keys, current_dict.dict_numreads,
                             basedir);

    current_dict.numkeys = detail::sort_and_deduplicate_keys(
        dictionary_keys, current_dict.dict_numreads);

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
    detail::write_hash_chunks(current_dict, basedir, dict_index);
  }

  // The remaining passes parallelize across dictionaries instead of reads.
  omp_set_num_threads(std::min(numdict, num_thr));
#pragma omp parallel
  {
#pragma omp for
    for (int dict_index = 0; dict_index < numdict; dict_index++) {
      bbhashdict &current_dict = dict[dict_index];
      // Count bucket sizes, then prefix-sum them into start positions.
      current_dict.startpos =
          new uint32_t[current_dict.numkeys +
                       1](); // 1 extra to store end pos of last key

      detail::count_bucket_sizes(current_dict, basedir, dict_index, num_thr);
      detail::finalize_bucket_offsets(current_dict);
      detail::populate_bucket_read_ids(current_dict, read_lengths, basedir,
                                       dict_index, num_thr);

      // Restore the prefix offsets after the insertion walk above.
      detail::restore_bucket_starts(current_dict);
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
