// Declares the BBHash-based dictionary structures and construction helpers used
// to index packed reads during reordering and encoding.

#ifndef SPRING_BITSET_DICTIONARY_H_
#define SPRING_BITSET_DICTIONARY_H_

#include <algorithm>
#include <bitset>
#include <cstdint>
#include <fstream>
#include <memory>
#include <omp.h>
#include <string>
#include <vector>

#include "core_utils.h"
#include "fs_utils.h"
#include "progress.h"
#include "single_phf.hpp"
#include "utils/bucketers.hpp"
#include "utils/encoders.hpp"
#include "utils/hasher.hpp"

namespace spring {

using boophf_t = pthash::single_phf<pthash::xxhash_64, pthash::range_bucketer,
                                    pthash::compact, false>;

inline std::string keys_bin_path(const std::string &base_dir,
                                 const int thread_id) {
  return base_dir + "/keys.bin." + std::to_string(thread_id);
}

inline std::string hash_bin_path(const std::string &base_dir,
                                 const int thread_id, const int dict_index) {
  return base_dir + "/hash.bin." + std::to_string(thread_id) + '.' +
         std::to_string(dict_index);
}

class bbhashdict {
public:
  std::unique_ptr<boophf_t> bphf;
  int start;
  int end;
  uint32_t numkeys;
  uint32_t dict_numreads; // Number of reads long enough to participate here.
  std::unique_ptr<uint32_t[]> startpos;
  std::unique_ptr<uint32_t[]> read_id;
  std::vector<bool> empty_bin;
  void findpos(int64_t *dictidx, const uint64_t &startposidx);
  void remove(const int64_t *dictidx, const uint64_t &startposidx,
              const int64_t read_id_to_remove);
  bbhashdict()
      : bphf(nullptr), start(0), end(0), numkeys(0), dict_numreads(0),
        startpos(nullptr), read_id(nullptr) {}
  bbhashdict(const bbhashdict &) = delete;
  bbhashdict &operator=(const bbhashdict &) = delete;
  bbhashdict(bbhashdict &&) noexcept = default;
  bbhashdict &operator=(bbhashdict &&) noexcept = default;
  ~bbhashdict() = default;
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
                             const int start_base, const uint32_t read_count,
                             const int bits_per_base,
                             uint64_t *dictionary_keys) {
  const std::bitset<bitset_size> local_index_mask = index_mask;
  const uint32_t local_read_count = read_count;
  const int local_bits_per_base = bits_per_base;
  const int local_start_base = start_base;
  std::bitset<bitset_size> *local_read_bits = read_bits;
  uint64_t *local_dictionary_keys = dictionary_keys;
#pragma omp parallel for default(none) shared(local_read_bits, local_dictionary_keys) \
    firstprivate(local_index_mask, local_read_count, local_bits_per_base, local_start_base)
  for (int64_t read_index = 0;
       read_index < static_cast<int64_t>(local_read_count);
       read_index++) {
    std::bitset<bitset_size> masked_read_bits =
        local_read_bits[read_index] & local_index_mask;
    local_dictionary_keys[read_index] =
        (masked_read_bits >> (local_bits_per_base * local_start_base)).to_ullong();
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
  const uint32_t local_key_count = key_count;
  const std::string local_base_dir = base_dir;
  const uint64_t *local_dictionary_keys = dictionary_keys;
#pragma omp parallel default(none) shared(local_dictionary_keys) \
    firstprivate(local_key_count, local_base_dir)
  {
    const int thread_id = omp_get_thread_num();
    const int thread_count = omp_get_num_threads();
    const thread_range range =
        split_thread_range(local_key_count, thread_id, thread_count);
    std::ofstream key_output(keys_bin_path(local_base_dir, thread_id),
                             std::ios::binary);

    for (uint64_t key_index = range.begin; key_index < range.end; key_index++)
      key_output.write(byte_ptr(&local_dictionary_keys[key_index]),
                       sizeof(uint64_t));
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
  const uint32_t local_num_reads = dictionary.dict_numreads;
  const std::string local_base_dir = base_dir;
  const int local_dict_index = dict_index;
  const bbhashdict *local_dictionary = &dictionary;
#pragma omp parallel default(none) shared(std::cerr, local_dictionary) \
    firstprivate(local_num_reads, local_base_dir, local_dict_index)
  {
    const int thread_id = omp_get_thread_num();
    const int thread_count = omp_get_num_threads();
    const thread_range range =
        split_thread_range(local_num_reads, thread_id, thread_count);
    const std::string key_path = keys_bin_path(local_base_dir, thread_id);
    const std::string hash_path =
        hash_bin_path(local_base_dir, thread_id, local_dict_index);
    std::ifstream key_input(key_path, std::ios::binary);
    std::ofstream hash_output(hash_path, std::ios::binary);
    uint64_t current_key;

    for (uint64_t key_index = range.begin; key_index < range.end; key_index++) {
      if (!key_input.read(byte_ptr(&current_key), sizeof(uint64_t))) {
        Logger::log_debug("block_id=dict-hash-write:" +
              std::to_string(local_dict_index) +
                          ", write_hash_chunks short read: path=" + key_path +
                          ", expected_bytes=" +
                          std::to_string(sizeof(uint64_t)) +
                          ", actual_bytes=" +
                          std::to_string(key_input.gcount()) +
                          ", index=" + std::to_string(key_index));
        std::cerr << "Error reading key at index " << key_index << " from "
                  << key_path << std::endl;
        break;
      }
      const uint64_t current_hash = (*(local_dictionary->bphf))(current_key);
      hash_output.write(byte_ptr(&current_hash), sizeof(uint64_t));
    }
    hash_output.flush();
    hash_output.close();

    key_input.close();
    safe_remove_file(key_path);
  }
}

inline void count_bucket_sizes(bbhashdict &dictionary,
                               const std::string &base_dir,
                               const int dict_index, const int thread_count) {
  uint64_t current_hash;
  for (int thread_id = 0; thread_id < thread_count; thread_id++) {
    std::string path = hash_bin_path(base_dir, thread_id, dict_index);
    std::ifstream hash_input(path, std::ios::binary);
    if (!hash_input.is_open()) {
      continue;
    }

    while (hash_input.read(byte_ptr(&current_hash), sizeof(uint64_t))) {
      if (current_hash >= dictionary.numkeys) {
        Logger::log_debug("block_id=dict-bucket-count:" +
              std::to_string(dict_index) +
                          ", count_bucket_sizes hash out-of-range: path=" +
                          path +
                          ", expected_bytes=" +
                          std::to_string(dictionary.numkeys) +
                          ", actual_bytes=" + std::to_string(current_hash) +
                          ", index=" + std::to_string(thread_id));
        continue;
      }
      dictionary.startpos[current_hash + 1]++;
    }
  }
}

inline void finalize_bucket_offsets(bbhashdict &dictionary) {
  dictionary.empty_bin.assign(dictionary.numkeys, false);
  for (uint32_t key_index = 0; key_index < dictionary.numkeys; key_index++) {
    if (dictionary.startpos[key_index + 1] == 0)
      dictionary.empty_bin[key_index] = true;
  }
  for (uint32_t key_index = 1; key_index < dictionary.numkeys; key_index++) {
    dictionary.startpos[key_index] += dictionary.startpos[key_index - 1];
  }
}

inline void
populate_bucket_read_ids(bbhashdict &dictionary, uint16_t *read_lengths,
                         const std::string &base_dir, const int dict_index,
                         const uint32_t numreads, const int thread_count) {
  dictionary.read_id = std::make_unique<uint32_t[]>(dictionary.dict_numreads);
  uint32_t read_index = 0;
  uint64_t current_hash;

  for (int thread_id = 0; thread_id < thread_count; thread_id++) {
    const std::string hash_path =
        hash_bin_path(base_dir, thread_id, dict_index);
    std::ifstream hash_input(hash_path, std::ios::binary);
    if (!hash_input.is_open()) {
      std::cerr << "Error: Could not open hash chunk for populating: "
                << hash_path << std::endl;
      continue;
    }
    while (hash_input.read(byte_ptr(&current_hash), sizeof(uint64_t))) {
      if (current_hash >= dictionary.numkeys) {
        Logger::log_debug(
          "block_id=dict-bucket-populate:" + std::to_string(dict_index) +
            ", populate_bucket_read_ids hash out-of-range: path=" +
            hash_path +
          ", expected_bytes=" + std::to_string(dictionary.numkeys) +
          ", actual_bytes=" + std::to_string(current_hash) +
          ", index=" + std::to_string(thread_id));
        continue;
      }
      while (read_index < numreads &&
             read_lengths[read_index] <= dictionary.end)
        read_index++;
      if (read_index < numreads) {
        dictionary.read_id[dictionary.startpos[current_hash]++] = read_index;
        read_index++;
      } else {
        Logger::log_debug(
          "block_id=dict-bucket-populate:" + std::to_string(dict_index) +
            ", populate_bucket_read_ids exhausted source reads: path=" +
            hash_path +
          ", expected_bytes=1, actual_bytes=0, index=" +
          std::to_string(thread_id));
        break;
      }
    }
    hash_input.close();
    safe_remove_file(hash_path);
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
void generateindexmasks(std::bitset<bitset_size> *index_masks, bbhashdict *dict,
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
                         const uint32_t numreads, const int bpb,
                         const std::string &basedir, const int num_thr) {
  auto index_masks = std::make_unique<std::bitset<bitset_size>[]>(
      static_cast<size_t>(numdict));
  generateindexmasks<bitset_size>(index_masks.get(), dict, numdict, bpb);

  for (int dict_index = 0; dict_index < numdict; dict_index++) {
    bbhashdict &current_dict = dict[dict_index];
    std::vector<uint64_t> dictionary_keys(numreads, 0);
    uint64_t *dictionary_keys_data = dictionary_keys.data();

    detail::compute_dictionary_keys<bitset_size>(read, index_masks[dict_index],
                                                 current_dict.start, numreads,
                                                 bpb, dictionary_keys_data);

    // Keep only reads that are long enough for this dictionary.
    current_dict.dict_numreads = detail::compact_dictionary_keys(
        dictionary_keys_data, read_lengths, numreads, current_dict.end);

    // Persist keys because later passes stream them again while building
    // the bucket layout.
    detail::write_key_chunks(dictionary_keys_data, current_dict.dict_numreads,
                             basedir);

    current_dict.numkeys = detail::sort_and_deduplicate_keys(
        dictionary_keys_data, current_dict.dict_numreads);

    Logger::log_info(std::string("Dictionary ") +
                     std::to_string(dict_index + 1) + " of " +
                     std::to_string(numdict) + ": Building MPHF for " +
                     std::to_string(current_dict.numkeys) + " keys...");
    pthash::build_configuration config;
    config.num_threads = 1;
    config.minimal = false;
    config.verbose = false;
    current_dict.bphf = std::make_unique<boophf_t>();
    Logger::log_info("  Building MPHF... ");
    current_dict.bphf->build_in_internal_memory(dictionary_keys_data,
                                                current_dict.numkeys, config);
    current_dict.numkeys = current_dict.bphf->table_size();
    Logger::log_info(std::string("Done. (T=") +
                     std::to_string(current_dict.numkeys) + ")");

    // Re-read the stored keys and materialize their hash buckets.
    Logger::log_info("  Writing hash chunks... ");
    detail::write_hash_chunks(current_dict, basedir, dict_index);
    Logger::log_info("Done.");
  }

  Logger::log_info("  Finalizing dictionaries (sequentially)...");
  for (int dict_index = 0; dict_index < numdict; dict_index++) {
    bbhashdict &current_dict = dict[dict_index];
    current_dict.startpos =
        std::make_unique<uint32_t[]>(current_dict.numkeys + 1);
    std::fill_n(current_dict.startpos.get(), current_dict.numkeys + 1, 0);
    detail::count_bucket_sizes(current_dict, basedir, dict_index, num_thr);
    detail::finalize_bucket_offsets(current_dict);
    detail::populate_bucket_read_ids(current_dict, read_lengths, basedir,
                                     dict_index, numreads, num_thr);
    detail::restore_bucket_starts(current_dict);
  }
  Logger::log_info("  Done finalization.");
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
