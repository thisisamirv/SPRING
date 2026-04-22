#ifndef SPRING_REORDER_IMPL_H_
#define SPRING_REORDER_IMPL_H_

#include "bitset_dictionary.h"
#include "core_utils.h"
#include "dna_utils.h"
#include "fs_utils.h"
#include "io_utils.h"
#include "params.h"
#include "progress.h"
#include "raii.h"
#include "reorder.h"
#include <algorithm>
#include <array>
#include <bitset>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <list>
#include <memory>
#include <numeric>
#include <omp.h>
#include <utility>
#include <vector>

namespace spring {

template <size_t bitset_size> struct reorder_global {
  uint32_t numreads;
  uint32_t numreads_array[2];

  int maxshift, num_thr, max_readlen;
  int numdict;

  std::string basedir;
  std::string infile[2];
  std::string outfile;
  std::string outfileRC;
  std::string outfileflag;
  std::string outfilepos;
  std::string outfileorder;
  std::string outfilereadlength;

  bool paired_end;
  bool methyl_ternary;

  std::vector<std::array<std::bitset<bitset_size>, 128>> basemask;
  std::vector<std::bitset<bitset_size> *> basemask_ptrs;
  std::bitset<bitset_size> mask64;
  reorder_global(int max_readlen_param)
      : numreads(0), numreads_array{0, 0}, maxshift(0), num_thr(0),
        max_readlen(max_readlen_param), numdict(NUM_DICT_REORDER),
        paired_end(false), methyl_ternary(false) {
    basemask.resize(static_cast<size_t>(max_readlen_param));
    basemask_ptrs.resize(static_cast<size_t>(max_readlen_param));
    for (int i = 0; i < max_readlen_param; i++)
      basemask_ptrs[i] = basemask[static_cast<size_t>(i)].data();
  }
  reorder_global(const reorder_global &) = delete;
  reorder_global &operator=(const reorder_global &) = delete;
  ~reorder_global() = default;
};

namespace detail {

inline std::string thread_output_path(const std::string &base_path,
                                      const int thread_id) {
  return base_path + '.' + std::to_string(thread_id);
}

inline std::string thread_singleton_output_path(const std::string &base_path,
                                                const int thread_id) {
  return base_path + ".singleton." + std::to_string(thread_id);
}

inline void
append_file_to_stream(std::ofstream &output_stream,
                      const std::string &input_path,
                      const std::ios::openmode mode = std::ios::in) {
  std::ifstream input_stream(input_path, mode);
  output_stream << input_stream.rdbuf();
  output_stream.clear();
}

} // namespace detail

inline void
initialize_reorder_dict_ranges(std::array<bbhashdict, NUM_DICT_REORDER> &dict,
                               const int max_readlen) {
  dict[0].start = max_readlen > 100 ? max_readlen / 2 - 32
                                    : max_readlen / 2 - max_readlen * 32 / 100;
  dict[0].end = max_readlen / 2 - 1;
  dict[1].start = max_readlen / 2;
  dict[1].end = max_readlen > 100
                    ? max_readlen / 2 - 1 + 32
                    : max_readlen / 2 - 1 + max_readlen * 32 / 100;
}

template <size_t bitset_size>
void bitsettostring(std::bitset<bitset_size> encoded_bases, char *read_chars,
                    const uint16_t readlen,
                    const reorder_global<bitset_size> &rg) {
  static const char reverse_base_lookup[4] = {'A', 'G', 'C', 'T'};
  unsigned long long packed_bases;
  for (int block_index = 0; block_index < 2 * readlen / 64 + 1; block_index++) {
    packed_bases = (encoded_bases & rg.mask64).to_ullong();
    encoded_bases >>= 64;
    for (int read_index = 32 * block_index;
         read_index < 32 * block_index + 32 && read_index < readlen;
         read_index++) {
      read_chars[read_index] = reverse_base_lookup[packed_bases % 4];
      packed_bases /= 4;
    }
  }
  read_chars[readlen] = '\0';
  return;
}

template <size_t bitset_size>
void setglobalarrays(reorder_global<bitset_size> &rg) {
  for (int i = 0; i < 64; i++)
    rg.mask64[i] = 1;
  for (int i = 0; i < rg.max_readlen; i++) {
    rg.basemask[i][(uint8_t)'A'][2 * i] = 0;
    rg.basemask[i][(uint8_t)'A'][2 * i + 1] = 0;
    rg.basemask[i][(uint8_t)'C'][2 * i] = 0;
    rg.basemask[i][(uint8_t)'C'][2 * i + 1] = 1;
    rg.basemask[i][(uint8_t)'G'][2 * i] = 1;
    rg.basemask[i][(uint8_t)'G'][2 * i + 1] = 0;
    rg.basemask[i][(uint8_t)'T'][2 * i] = 1;
    rg.basemask[i][(uint8_t)'T'][2 * i + 1] = 1;
  }
  return;
}

template <size_t bitset_size>
void updaterefcount(std::bitset<bitset_size> &current_read,
                    std::bitset<bitset_size> &reference_read,
                    std::bitset<bitset_size> &reverse_reference_read,
                    int **base_counts, const bool reset_counts,
                    const bool use_reverse_orientation, const int shift,
                    const uint16_t current_read_length, int &reference_length,
                    const reorder_global<bitset_size> &rg) {
  static const char base_lookup[4] = {'A', 'C', 'T', 'G'};
  auto base_to_index = [](uint8_t encoded_base) {
    return (encoded_base & 0x06) >> 1;
  }; // inverse of above
  char read_chars[MAX_READ_LEN + 1];
  char reverse_chars[MAX_READ_LEN + 1];
  char *oriented_read;
  bitsettostring<bitset_size>(current_read, read_chars, current_read_length,
                              rg);
  if (!use_reverse_orientation)
    oriented_read = read_chars;
  else {
    reverse_complement(read_chars, reverse_chars, current_read_length);
    oriented_read = reverse_chars;
  }

  if (reset_counts == true) {
    std::fill(base_counts[0], base_counts[0] + rg.max_readlen, 0);
    std::fill(base_counts[1], base_counts[1] + rg.max_readlen, 0);
    std::fill(base_counts[2], base_counts[2] + rg.max_readlen, 0);
    std::fill(base_counts[3], base_counts[3] + rg.max_readlen, 0);
    for (int read_index = 0; read_index < current_read_length; read_index++) {
      base_counts[base_to_index((uint8_t)oriented_read[read_index])]
                 [read_index] = 1;
    }
    reference_length = current_read_length;
  } else {
    if (!use_reverse_orientation) {
      // shift count
      for (int ref_index = 0; ref_index < reference_length - shift;
           ref_index++) {
        for (int base_index = 0; base_index < 4; base_index++)
          base_counts[base_index][ref_index] =
              base_counts[base_index][ref_index + shift];
        if (ref_index < current_read_length)
          base_counts[base_to_index((uint8_t)oriented_read[ref_index])]
                     [ref_index] += 1;
      }

      // for the new positions set count to 1
      for (int ref_index = reference_length - shift;
           ref_index < current_read_length; ref_index++) {
        for (int base_index = 0; base_index < 4; base_index++)
          base_counts[base_index][ref_index] = 0;
        base_counts[base_to_index((uint8_t)oriented_read[ref_index])]
                   [ref_index] = 1;
      }
      reference_length =
          std::max<int>(reference_length - shift, current_read_length);
    } else {
      if (current_read_length - shift >= reference_length) {
        for (int ref_index = current_read_length - shift - reference_length;
             ref_index < current_read_length - shift; ref_index++) {
          for (int base_index = 0; base_index < 4; base_index++)
            base_counts[base_index][ref_index] =
                base_counts[base_index][ref_index - (current_read_length -
                                                     shift - reference_length)];
          base_counts[base_to_index((uint8_t)oriented_read[ref_index])]
                     [ref_index] += 1;
        }
        for (int ref_index = 0;
             ref_index < current_read_length - shift - reference_length;
             ref_index++) {
          for (int base_index = 0; base_index < 4; base_index++)
            base_counts[base_index][ref_index] = 0;
          base_counts[base_to_index((uint8_t)oriented_read[ref_index])]
                     [ref_index] = 1;
        }
        for (int ref_index = current_read_length - shift;
             ref_index < current_read_length; ref_index++) {
          for (int base_index = 0; base_index < 4; base_index++)
            base_counts[base_index][ref_index] = 0;
          base_counts[base_to_index((uint8_t)oriented_read[ref_index])]
                     [ref_index] = 1;
        }
        reference_length = current_read_length;
      } else if (reference_length + shift <= rg.max_readlen) {
        for (int ref_index = reference_length - current_read_length + shift;
             ref_index < reference_length; ref_index++)
          base_counts[base_to_index((
              uint8_t)oriented_read[ref_index - (reference_length -
                                                 current_read_length + shift)])]
                     [ref_index] += 1;
        for (int ref_index = reference_length;
             ref_index < reference_length + shift; ref_index++) {
          for (int base_index = 0; base_index < 4; base_index++)
            base_counts[base_index][ref_index] = 0;
          base_counts[base_to_index((
              uint8_t)oriented_read[ref_index - (reference_length -
                                                 current_read_length + shift)])]
                     [ref_index] = 1;
        }
        reference_length = reference_length + shift;
      } else {
        for (int ref_index = 0; ref_index < rg.max_readlen - shift;
             ref_index++) {
          for (int base_index = 0; base_index < 4; base_index++)
            base_counts[base_index][ref_index] =
                base_counts[base_index][ref_index + (reference_length + shift -
                                                     rg.max_readlen)];
        }
        for (int ref_index = rg.max_readlen - current_read_length;
             ref_index < rg.max_readlen - shift; ref_index++)
          base_counts[base_to_index(
              (uint8_t)oriented_read[ref_index -
                                     (rg.max_readlen - current_read_length)])]
                     [ref_index] += 1;
        for (int ref_index = rg.max_readlen - shift; ref_index < rg.max_readlen;
             ref_index++) {
          for (int base_index = 0; base_index < 4; base_index++)
            base_counts[base_index][ref_index] = 0;
          base_counts[base_to_index(
              (uint8_t)oriented_read[ref_index -
                                     (rg.max_readlen - current_read_length)])]
                     [ref_index] = 1;
        }
        reference_length = rg.max_readlen;
      }
    }
    for (int ref_index = 0; ref_index < reference_length; ref_index++) {
      int best_base_count = 0;
      int best_base_index = 0;
      for (int base_index = 0; base_index < 4; base_index++)
        if (base_counts[base_index][ref_index] > best_base_count) {
          best_base_count = base_counts[base_index][ref_index];
          best_base_index = base_index;
        }
      oriented_read[ref_index] = base_lookup[best_base_index];
    }
  }
  chartobitset<bitset_size>(
      oriented_read, reference_length, reference_read,
      const_cast<std::bitset<bitset_size> **>(rg.basemask_ptrs.data()));
  char reverse_oriented_read[MAX_READ_LEN + 1];
  reverse_complement(oriented_read, reverse_oriented_read, reference_length);
  chartobitset<bitset_size>(
      reverse_oriented_read, reference_length, reverse_reference_read,
      const_cast<std::bitset<bitset_size> **>(rg.basemask_ptrs.data()));

  return;
}

template <size_t bitset_size>
void readDnaFile(std::bitset<bitset_size> *read, uint16_t *read_lengths,
                 const reorder_global<bitset_size> &rg) {
  if (rg.methyl_ternary) {
    std::string s;
    std::ifstream f(rg.infile[0], std::ifstream::in | std::ios::binary);
    for (uint32_t i = 0; i < rg.numreads_array[0]; i++) {
      read_dna_ternary_bits(s, f);
      read_lengths[i] = static_cast<uint16_t>(s.size());
      chartobitset<bitset_size>(
          s.data(), read_lengths[i], read[i],
          const_cast<std::bitset<bitset_size> **>(rg.basemask_ptrs.data()));
    }
    f.close();
    safe_remove_file(rg.infile[0]);
    if (rg.paired_end) {
      std::ifstream f(rg.infile[1], std::ifstream::in | std::ios::binary);
      for (uint32_t i = rg.numreads_array[0];
           i < rg.numreads_array[0] + rg.numreads_array[1]; i++) {
        read_dna_ternary_bits(s, f);
        read_lengths[i] = static_cast<uint16_t>(s.size());
        chartobitset<bitset_size>(
            s.data(), read_lengths[i], read[i],
            const_cast<std::bitset<bitset_size> **>(rg.basemask_ptrs.data()));
      }
      f.close();
      safe_remove_file(rg.infile[1]);
    }
    return;
  }
  std::ifstream f(rg.infile[0], std::ifstream::in | std::ios::binary);
  for (uint32_t i = 0; i < rg.numreads_array[0]; i++) {
    f.read(byte_ptr(&read_lengths[i]), sizeof(uint16_t));
    uint16_t num_bytes_to_read = ((uint32_t)read_lengths[i] + 4 - 1) / 4;
    f.read(byte_ptr(&read[i]), num_bytes_to_read);
  }
  f.close();
  safe_remove_file(rg.infile[0]);
  if (rg.paired_end) {
    std::ifstream f(rg.infile[1], std::ifstream::in | std::ios::binary);
    for (uint32_t i = rg.numreads_array[0];
         i < rg.numreads_array[0] + rg.numreads_array[1]; i++) {
      f.read(byte_ptr(&read_lengths[i]), sizeof(uint16_t));
      uint16_t num_bytes_to_read = ((uint32_t)read_lengths[i] + 4 - 1) / 4;
      f.read(byte_ptr(&read[i]), num_bytes_to_read);
    }
    f.close();
    safe_remove_file(rg.infile[1]);
  }
  return;
}

namespace detail {
template <size_t bitset_size>
bool search_match(const std::bitset<bitset_size> &ref,
                  std::bitset<bitset_size> *index_masks, OmpLock *dict_locks,
                  OmpLock *read_locks, std::bitset<bitset_size> **length_masks,
                  uint16_t *read_lengths, bool *remaining_reads,
                  std::bitset<bitset_size> *reads, bbhashdict *dict,
                  uint32_t &matched_read_id, const bool use_reverse_match,
                  const int shift, const int &ref_len,
                  const reorder_global<bitset_size> &rg) {
  static const unsigned int thresh = THRESH_REORDER;
  const int maxsearch = MAX_SEARCH_REORDER;
  std::bitset<bitset_size> masked_ref_bits;
  uint64_t lookup_key;
  int64_t bucket_range[2];
  uint64_t bucket_start_index;
  std::array<uint32_t, MAX_SEARCH_REORDER> candidate_ids{};
  bool found_match = 0;
  for (int dictionary_index = 0; dictionary_index < rg.numdict;
       dictionary_index++) {
    if (!use_reverse_match) {
      if (dict[dictionary_index].end + shift >= ref_len)
        continue;
    } else {
      if (dict[dictionary_index].end >= ref_len + shift ||
          dict[dictionary_index].start <= shift)
        continue;
    }
    masked_ref_bits = ref & index_masks[dictionary_index];
    lookup_key =
        (masked_ref_bits >> 2 * dict[dictionary_index].start).to_ullong();
    bucket_start_index = (*dict[dictionary_index].bphf)(lookup_key);
    if (bucket_start_index >= dict[dictionary_index].numkeys)
      continue;
    if (!omp_test_lock(
            dict_locks[detail::lock_shard(bucket_start_index)].get()))
      continue;

    size_t candidate_count = 0;
    bool bucket_matches_lookup = false;
    dict[dictionary_index].findpos(bucket_range, bucket_start_index);
    if (dict[dictionary_index].empty_bin[bucket_start_index]) {
      omp_unset_lock(dict_locks[detail::lock_shard(bucket_start_index)].get());
      continue;
    }
    uint64_t candidate_key =
        ((reads[dict[dictionary_index].read_id[bucket_range[0]]] &
          index_masks[dictionary_index]) >>
         2 * dict[dictionary_index].start)
            .to_ullong();
    if (lookup_key == candidate_key) {
      bucket_matches_lookup = true;
      for (int64_t bucket_index = bucket_range[1] - 1;
           bucket_index >= bucket_range[0] &&
           bucket_index >= bucket_range[1] - maxsearch;
           bucket_index--) {
        if (candidate_count >= candidate_ids.size())
          break;
        candidate_ids[candidate_count++] =
            dict[dictionary_index].read_id[bucket_index];
      }
    }
    omp_unset_lock(dict_locks[detail::lock_shard(bucket_start_index)].get());

    if (!bucket_matches_lookup)
      continue;

    for (size_t candidate_index = 0; candidate_index < candidate_count;
         candidate_index++) {
      const auto read_id = candidate_ids[candidate_index];
      size_t hamming;
      if (!use_reverse_match)
        hamming = ((ref ^ reads[read_id]) &
                   length_masks[0][rg.max_readlen -
                                   std::min<int>(ref_len - shift,
                                                 read_lengths[read_id])])
                      .count();
      else
        hamming = ((ref ^ reads[read_id]) &
                   length_masks[shift][rg.max_readlen -
                                       std::min<int>(ref_len + shift,
                                                     read_lengths[read_id])])
                      .count();
      if (hamming > thresh)
        continue;

      if (!omp_test_lock(read_locks[detail::lock_shard(read_id)].get()))
        continue;
      if (remaining_reads[read_id]) {
        remaining_reads[read_id] = 0;
        matched_read_id = read_id;
        found_match = 1;
      }
      omp_unset_lock(read_locks[detail::lock_shard(read_id)].get());
      if (found_match == 1)
        break;
    }

    if (found_match == 1)
      break;
  }
  return found_match;
}
} // namespace detail

template <size_t bitset_size>
void reorder(std::bitset<bitset_size> *read, bbhashdict *dict,
             uint16_t *read_lengths, const reorder_global<bitset_size> &rg) {
  const uint32_t num_locks = NUM_LOCKS_REORDER;
  std::vector<OmpLock> dict_locks(num_locks);
  std::vector<OmpLock> read_locks(num_locks);
  std::vector<OmpLock> remaining_read_lock(num_locks);
  std::vector<std::vector<std::bitset<bitset_size>>> length_masks;
  length_masks.assign(static_cast<size_t>(rg.max_readlen),
                      std::vector<std::bitset<bitset_size>>(rg.max_readlen));
  std::vector<std::bitset<bitset_size> *> length_masks_ptrs;
  length_masks_ptrs.reserve(static_cast<size_t>(rg.max_readlen));
  for (int i = 0; i < rg.max_readlen; ++i)
    length_masks_ptrs.push_back(length_masks[static_cast<size_t>(i)].data());
  generatemasks<bitset_size>(length_masks_ptrs.data(), rg.max_readlen, 2);
  std::vector<std::bitset<bitset_size>> index_masks(
      static_cast<size_t>(rg.numdict));
  SPRING_LOG_INFO("Constructing dictionaries");
  generateindexmasks<bitset_size>(index_masks.data(), dict, rg.numdict, 2);
  auto remaining_reads_storage = std::make_unique<bool[]>(rg.numreads);
  bool *remaining_reads = remaining_reads_storage.get();
  std::fill(remaining_reads, remaining_reads + rg.numreads, true);

  uint32_t first_read = 0;
  SPRING_LOG_INFO("Reordering reads");
  std::vector<uint32_t> unmatched_counts(static_cast<size_t>(rg.num_thr));
  std::vector<std::string> open_stream_errors(static_cast<size_t>(rg.num_thr));
#pragma omp parallel default(none)                                             \
    shared(rg, read, read_lengths, dict, dict_locks, read_locks,               \
               remaining_read_lock, length_masks_ptrs, index_masks,            \
               remaining_reads, unmatched_counts, open_stream_errors,          \
               std::cerr, std::cout, first_read)
  {
    bool done = false;
    int thread_id = omp_get_thread_num();
    const int64_t scan_stride = std::max<int64_t>(1, omp_get_num_threads());
    int64_t remaining_read_scan = rg.numreads - 1 - thread_id;
    if (remaining_read_scan < 0)
      remaining_read_scan = -1;
    std::ofstream orientation_output(
        detail::thread_output_path(rg.outfileRC, thread_id), std::ios::binary);
    std::ofstream flag_output(
        detail::thread_output_path(rg.outfileflag, thread_id),
        std::ios::binary);
    std::ofstream position_output(
        detail::thread_output_path(rg.outfilepos, thread_id), std::ios::binary);
    std::ofstream order_output(
        detail::thread_output_path(rg.outfileorder, thread_id),
        std::ios::binary);
    std::ofstream singleton_order_output(
        detail::thread_singleton_output_path(rg.outfileorder, thread_id),
        std::ios::binary);
    std::ofstream read_length_output(
        detail::thread_output_path(rg.outfilereadlength, thread_id),
        std::ios::binary);
    if (!orientation_output.is_open() || !flag_output.is_open() ||
        !position_output.is_open() || !order_output.is_open() ||
        !singleton_order_output.is_open() || !read_length_output.is_open()) {
      std::string error_msg = "Thread " + std::to_string(thread_id) +
                              ": Failed to open one or more temporary "
                              "files in reorder. Working directory: " +
                              rg.basedir;
      open_stream_errors[static_cast<size_t>(thread_id)] = error_msg;
      done = true;
    }

    unmatched_counts[thread_id] = 0;
    std::bitset<bitset_size> reference_read, reverse_reference_read,
        masked_read_bits;

    int64_t seed_read_id;

    std::array<std::list<std::pair<uint32_t, uint64_t>>, NUM_DICT_REORDER>
        pending_bin_deletions;

    bool stop_searching = false;
    uint32_t thread_read_count = 0;
    uint32_t unmatched_reads_in_window = 0;

    std::array<std::unique_ptr<int[]>, 4> base_counts_storage;
    std::array<int *, 4> base_counts;
    for (int base_index = 0; base_index < 4; base_index++) {
      base_counts_storage[static_cast<size_t>(base_index)] =
          std::make_unique<int[]>(static_cast<size_t>(rg.max_readlen));
      base_counts[static_cast<size_t>(base_index)] =
          base_counts_storage[static_cast<size_t>(base_index)].get();
    }
    int64_t bucket_range[2];
    uint64_t bucket_start_index;
    bool found_match = 0;
    bool previous_read_unmatched = false;
    bool left_search_start = false;
    bool left_search = false;
    int64_t current_read_id;
    int64_t previous_read_id;
    uint64_t lookup_key;
    int reference_length;
    int64_t reference_position;
    int64_t current_read_position;

    // Claim an initial seed read from a shared cursor. Keep trying until we
    // either reserve an unclaimed read or exhaust the input.
    while (!done) {
#pragma omp atomic capture
      {
        current_read_id = first_read;
        first_read += 1;
      }
      if (rg.numreads == 0 ||
          current_read_id >= static_cast<int64_t>(rg.numreads)) {
        done = true;
        break;
      }

      omp_set_lock(
          remaining_read_lock[detail::lock_shard(current_read_id)].get());
      omp_set_lock(read_locks[detail::lock_shard(current_read_id)].get());
      if (remaining_reads[current_read_id]) {
        remaining_reads[current_read_id] = 0;
        unmatched_counts[thread_id]++;
        omp_unset_lock(read_locks[detail::lock_shard(current_read_id)].get());
        omp_unset_lock(
            remaining_read_lock[detail::lock_shard(current_read_id)].get());
        break;
      }
      omp_unset_lock(read_locks[detail::lock_shard(current_read_id)].get());
      omp_unset_lock(
          remaining_read_lock[detail::lock_shard(current_read_id)].get());
    }
    if (!done) {
      updaterefcount<bitset_size>(read[current_read_id], reference_read,
                                  reverse_reference_read, base_counts.data(),
                                  true, false, 0, read_lengths[current_read_id],
                                  reference_length, rg);
      current_read_position = 0;
      reference_position = 0;
      seed_read_id = current_read_id;
      previous_read_unmatched = true;
      previous_read_id = current_read_id;
    }
    while (!done) {
      if (thread_read_count % 100000 == 0) {
        if (thread_id == 0) {
          if (auto *progress = ProgressBar::GlobalInstance()) {
            // Estimate progress based on current scan position
            // This is a rough estimate but avoids atomic overhead in the hot
            // loop
            progress->update(1.0f - (float)remaining_read_scan / rg.numreads);
          }
        }
        if (unmatched_reads_in_window > STOP_CRITERIA_REORDER * 1000000) {
          stop_searching = true;
        }
        unmatched_reads_in_window = 0;
      }
      thread_read_count++;
      for (int dictionary_index = 0; dictionary_index < rg.numdict;
           dictionary_index++) {
        for (auto pending_delete_it =
                 pending_bin_deletions[dictionary_index].begin();
             pending_delete_it !=
             pending_bin_deletions[dictionary_index].end();) {
          uint32_t read_id = (*pending_delete_it).first;
          uint64_t pending_bucket_start = (*pending_delete_it).second;
          if (!omp_test_lock(
                  dict_locks[detail::lock_shard(pending_bucket_start)].get())) {
            ++pending_delete_it;
            continue;
          }
          dict[dictionary_index].findpos(bucket_range, pending_bucket_start);
          dict[dictionary_index].remove(bucket_range, pending_bucket_start,
                                        read_id);
          pending_delete_it =
              pending_bin_deletions[dictionary_index].erase(pending_delete_it);
          omp_unset_lock(
              dict_locks[detail::lock_shard(pending_bucket_start)].get());
        }
      }

      if (!left_search_start) {
        for (int dictionary_index = 0; dictionary_index < rg.numdict;
             dictionary_index++) {
          if (read_lengths[current_read_id] <= dict[dictionary_index].end)
            continue;
          masked_read_bits =
              read[current_read_id] & index_masks[dictionary_index];
          lookup_key = (masked_read_bits >> 2 * dict[dictionary_index].start)
                           .to_ullong();
          bucket_start_index = (*dict[dictionary_index].bphf)(lookup_key);
          if (bucket_start_index >= dict[dictionary_index].numkeys ||
              dict[dictionary_index].empty_bin[bucket_start_index])
            continue;
          if (!omp_test_lock(
                  dict_locks[detail::lock_shard(bucket_start_index)].get())) {
            pending_bin_deletions[dictionary_index].push_back(
                std::make_pair(current_read_id, bucket_start_index));
            continue;
          }
          dict[dictionary_index].findpos(bucket_range, bucket_start_index);
          dict[dictionary_index].remove(bucket_range, bucket_start_index,
                                        current_read_id);
          omp_unset_lock(
              dict_locks[detail::lock_shard(bucket_start_index)].get());
        }
      } else {
        left_search_start = false;
      }
      found_match = 0;
      uint32_t matched_read_id;
      if (!stop_searching &&
          (reference_position < MAX_CONTIG_GROWTH - 2 * rg.max_readlen))
        for (int shift = 0; shift < rg.maxshift; shift++) {
          found_match = detail::search_match<bitset_size>(
              reference_read, index_masks.data(), dict_locks.data(),
              read_locks.data(), length_masks_ptrs.data(), read_lengths,
              remaining_reads, read, dict, matched_read_id, false, shift,
              reference_length, rg);
          if (found_match == 1) {
            current_read_id = matched_read_id;
            int previous_reference_length = reference_length;
            updaterefcount<bitset_size>(
                read[current_read_id], reference_read, reverse_reference_read,
                base_counts.data(), false, false, shift,
                read_lengths[current_read_id], reference_length, rg);
            if (!left_search) {
              current_read_position = reference_position + shift;
              reference_position = current_read_position;
            } else {
              current_read_position = reference_position +
                                      previous_reference_length - shift -
                                      read_lengths[current_read_id];
              reference_position = reference_position +
                                   previous_reference_length - shift -
                                   reference_length;
            }
            if (previous_read_unmatched == true) {
              orientation_output.put('d');
              order_output.write(byte_ptr(&previous_read_id), sizeof(uint32_t));
              flag_output.put('0');
              int64_t zero = 0;
              position_output.write(byte_ptr(&zero), sizeof(int64_t));
              read_length_output.write(
                  byte_ptr(&read_lengths[previous_read_id]), sizeof(uint16_t));
            }
            orientation_output.put(left_search ? 'r' : 'd');
            order_output.write(byte_ptr(&current_read_id), sizeof(uint32_t));
            flag_output.put('1');
            position_output.write(byte_ptr(&current_read_position),
                                  sizeof(int64_t));
            read_length_output.write(byte_ptr(&read_lengths[current_read_id]),
                                     sizeof(uint16_t));

            previous_read_unmatched = false;
            break;
          }

          // find reverse match
          found_match = detail::search_match<bitset_size>(
              reverse_reference_read, index_masks.data(), dict_locks.data(),
              read_locks.data(), length_masks_ptrs.data(), read_lengths,
              remaining_reads, read, dict, matched_read_id, true, shift,
              reference_length, rg);
          if (found_match == 1) {
            current_read_id = matched_read_id;
            int previous_reference_length = reference_length;
            updaterefcount<bitset_size>(
                read[current_read_id], reference_read, reverse_reference_read,
                base_counts.data(), false, true, shift,
                read_lengths[current_read_id], reference_length, rg);
            if (!left_search) {
              current_read_position = reference_position +
                                      previous_reference_length + shift -
                                      read_lengths[current_read_id];
              reference_position = reference_position +
                                   previous_reference_length + shift -
                                   reference_length;
            } else {
              current_read_position = reference_position - shift;
              reference_position = current_read_position;
            }
            if (previous_read_unmatched ==
                true) // prev read not singleton, write it now
            {
              orientation_output.put('d');
              order_output.write(byte_ptr(&previous_read_id), sizeof(uint32_t));
              flag_output.put('0');
              int64_t zero = 0;
              position_output.write(byte_ptr(&zero), sizeof(int64_t));
              read_length_output.write(
                  byte_ptr(&read_lengths[previous_read_id]), sizeof(uint16_t));
            }
            orientation_output.put(left_search ? 'd' : 'r');
            order_output.write(byte_ptr(&current_read_id), sizeof(uint32_t));
            flag_output.put('1');
            position_output.write(byte_ptr(&current_read_position),
                                  sizeof(int64_t));
            read_length_output.write(byte_ptr(&read_lengths[current_read_id]),
                                     sizeof(uint16_t));

            previous_read_unmatched = false;
            break;
          }

          reverse_reference_read <<= 2;
          reference_read >>= 2;
        }
      if (found_match == 0) {
        unmatched_reads_in_window++;
        if (!left_search) {
          // Retry around the contig seed in reverse-complement space once.
          left_search = true;
          left_search_start = true;
          updaterefcount<bitset_size>(
              read[seed_read_id], reference_read, reverse_reference_read,
              base_counts.data(), true, true, 0, read_lengths[seed_read_id],
              reference_length, rg);
          reference_position = 0;
          current_read_position = 0;
        } else {
          left_search = false;
          for (int64_t read_id = remaining_read_scan; read_id >= 0;
               read_id -= scan_stride) {
            omp_set_lock(
                remaining_read_lock[detail::lock_shard(read_id)].get());
            omp_set_lock(read_locks[detail::lock_shard(read_id)].get());
            if (remaining_reads[read_id]) {
              current_read_id = read_id;
              remaining_read_scan = read_id - scan_stride;
              remaining_reads[read_id] = 0;
              found_match = 1;
              unmatched_counts[thread_id]++;
            }
            omp_unset_lock(read_locks[detail::lock_shard(read_id)].get());
            omp_unset_lock(
                remaining_read_lock[detail::lock_shard(read_id)].get());
            if (found_match == 1)
              break;
          }
          if (found_match == 0) {
            if (previous_read_unmatched == true) {
              singleton_order_output.write(byte_ptr(&previous_read_id),
                                           sizeof(uint32_t));
            }
            done = 1;
          } else {
            updaterefcount<bitset_size>(
                read[current_read_id], reference_read, reverse_reference_read,
                base_counts.data(), true, false, 0,
                read_lengths[current_read_id], reference_length, rg);
            reference_position = 0;
            current_read_position = 0;
            if (previous_read_unmatched == true) {
              singleton_order_output.write(byte_ptr(&previous_read_id),
                                           sizeof(uint32_t));
            }
            previous_read_unmatched = true;
            seed_read_id = current_read_id;
            previous_read_id = current_read_id;
          }
        }
      }
    }

    orientation_output.close();
    order_output.close();
    flag_output.close();
    position_output.close();
    singleton_order_output.close();
    read_length_output.close();
    // base_counts_storage RAII will free the per-thread buffers
  }
  // Safety net: any reads still marked remaining were never emitted by worker
  // threads. Append them as singletons so downstream stages see all reads.
  uint32_t recovered_singletons = 0;
  {
    std::ofstream recovered_singleton_output(
        detail::thread_singleton_output_path(rg.outfileorder, 0),
        std::ios::binary | std::ios::app);
    for (uint32_t read_id = 0; read_id < rg.numreads; read_id++) {
      if (!remaining_reads[read_id])
        continue;
      recovered_singleton_output.write(byte_ptr(&read_id), sizeof(uint32_t));
      remaining_reads[read_id] = false;
      recovered_singletons++;
    }
  }

  if (recovered_singletons > 0) {
    unmatched_counts[0] += recovered_singletons;
    SPRING_LOG_DEBUG("Recovered leftover reorder reads as singletons: " +
                     std::to_string(recovered_singletons));
  }

  for (const std::string &error_msg : open_stream_errors) {
    if (!error_msg.empty()) {
      std::cerr << error_msg << std::endl;
    }
  }
  // remaining_reads_storage RAII will free the remaining_reads buffer
  SPRING_LOG_INFO("Reordering done, " +
                  std::to_string(std::accumulate(unmatched_counts.begin(),
                                                 unmatched_counts.end(), 0)) +
                  " were unmatched");
  // length_masks and index_masks are now RAII-managed containers
  return;
}

template <size_t bitset_size>
void writetofile(std::bitset<bitset_size> *read, uint16_t *read_lengths,
                 reorder_global<bitset_size> &rg) {
  std::vector<uint32_t> numreads_s_thr(rg.num_thr, 0);
  // Each thread materializes its reordered reads before the singleton merge
  // step.
#pragma omp parallel default(none)                                             \
    shared(rg, read, read_lengths, numreads_s_thr, std::cerr, std::cout)
  {
    int tid = omp_get_thread_num();
    std::ofstream fout(detail::thread_output_path(rg.outfile, tid),
                       std::ofstream::out | std::ios::binary);
    std::ofstream fout_s(detail::thread_singleton_output_path(rg.outfile, tid),
                         std::ofstream::out | std::ios::binary);
    gzip_istream inRC(detail::thread_output_path(rg.outfileRC, tid));
    std::ifstream finorder(detail::thread_output_path(rg.outfileorder, tid),
                           std::ifstream::in | std::ios::binary);
    std::ifstream finorder_s(
        detail::thread_singleton_output_path(rg.outfileorder, tid),
        std::ifstream::in | std::ios::binary);
    char s[MAX_READ_LEN + 1], s1[MAX_READ_LEN + 1];
    uint32_t current;
    char c;
    while (inRC.get(c)) {
      finorder.read(byte_ptr(&current), sizeof(uint32_t));
      if (c == 'd') {
        if (rg.methyl_ternary) {
          bitsettostring<bitset_size>(read[current], s, read_lengths[current],
                                      rg);
          write_dna_ternary_bits(s, fout);
        } else {
          uint16_t num_bytes_to_write =
              ((uint32_t)read_lengths[current] + 4 - 1) / 4;
          fout.write(byte_ptr(&read_lengths[current]), sizeof(uint16_t));
          fout.write(byte_ptr(&read[current]), num_bytes_to_write);
        }
      } else {
        bitsettostring<bitset_size>(read[current], s, read_lengths[current],
                                    rg);
        reverse_complement(s, s1, read_lengths[current]);
        if (rg.methyl_ternary) {
          write_dna_ternary_bits(s1, fout);
        } else {
          write_dna_in_bits(s1, fout);
        }
      }
    }
    finorder_s.read(byte_ptr(&current), sizeof(uint32_t));
    while (!finorder_s.eof()) {
      numreads_s_thr[tid]++;
      if (rg.methyl_ternary) {
        bitsettostring<bitset_size>(read[current], s, read_lengths[current],
                                    rg);
        write_dna_ternary_bits(s, fout_s);
      } else {
        uint16_t num_bytes_to_write =
            ((uint32_t)read_lengths[current] + 4 - 1) / 4;
        fout_s.write(byte_ptr(&read_lengths[current]), sizeof(uint16_t));
        fout_s.write(byte_ptr(&read[current]), num_bytes_to_write);
      }
      finorder_s.read(byte_ptr(&current), sizeof(uint32_t));
    }
    fout.close();
    fout_s.close();
    inRC.close();
    finorder.close();
    finorder_s.close();
  }

  uint32_t numreads_s = 0;
  for (int i = 0; i < rg.num_thr; i++)
    numreads_s += numreads_s_thr[i];
  // write numreads_s to a file
  std::ofstream fout_s_count(rg.outfile + ".singleton" + ".count",
                             std::ios::out | std::ios::binary);
  fout_s_count.write(byte_ptr(&numreads_s), sizeof(uint32_t));
  fout_s_count.close();

  std::ofstream fout_s(rg.outfile + ".singleton",
                       std::ofstream::out | std::ios::binary);
  std::ofstream foutorder_s(rg.outfileorder + ".singleton",
                            std::ofstream::out | std::ios::binary);
  for (int tid = 0; tid < rg.num_thr; tid++) {
    const std::string singleton_read_path =
        detail::thread_singleton_output_path(rg.outfile, tid);
    const std::string singleton_order_path =
        detail::thread_singleton_output_path(rg.outfileorder, tid);

    detail::append_file_to_stream(fout_s, singleton_read_path,
                                  std::ios::binary);
    detail::append_file_to_stream(foutorder_s, singleton_order_path,
                                  std::ios::binary);

    safe_remove_file(singleton_read_path);
    safe_remove_file(singleton_order_path);
  }
  fout_s.close();
  foutorder_s.close();
  return;
}

template <size_t bitset_size>
void reorder_main(const std::string &temp_dir, const compression_params &cp) {
  reorder_global<bitset_size> rg(cp.read_info.max_readlen);
  rg.basedir = temp_dir;
  rg.infile[0] = rg.basedir + "/input_clean_1.dna";
  rg.infile[1] = rg.basedir + "/input_clean_2.dna";
  rg.outfile = rg.basedir + "/temp.dna";
  rg.outfileRC = rg.basedir + "/read_rev.txt";
  rg.outfileflag = rg.basedir + "/tempflag.txt";
  rg.outfilepos = rg.basedir + "/temppos.txt";
  rg.outfileorder = rg.basedir + "/read_order.bin";
  rg.outfilereadlength = rg.basedir + "/read_lengths.bin";

  rg.max_readlen = cp.read_info.max_readlen;
  rg.num_thr = cp.encoding.num_thr;
  rg.paired_end = cp.encoding.paired_end;
  rg.methyl_ternary = cp.encoding.methyl_ternary;
  rg.maxshift = rg.max_readlen / 2;
  std::array<bbhashdict, NUM_DICT_REORDER> dict;
  initialize_reorder_dict_ranges(dict, rg.max_readlen);

  rg.numreads =
      cp.read_info.num_reads_clean[0] + cp.read_info.num_reads_clean[1];
  rg.numreads_array[0] = cp.read_info.num_reads_clean[0];
  rg.numreads_array[1] = cp.read_info.num_reads_clean[1];
  rg.numdict =
      (rg.numreads < DICT_SINGLE_STAGE_READ_THRESHOLD) ? 1 : NUM_DICT_REORDER;
  SPRING_LOG_DEBUG("Reorder dictionary configuration: active_dicts=" +
                   std::to_string(rg.numdict) +
                   ", clean_reads=" + std::to_string(rg.numreads));

  omp_set_num_threads(rg.num_thr);
  setglobalarrays(rg);
  std::vector<std::bitset<bitset_size>> read;
  std::vector<uint16_t> read_lengths;
  read.resize(static_cast<size_t>(rg.numreads));
  read_lengths.resize(static_cast<size_t>(rg.numreads));
  SPRING_LOG_INFO("Reading file");
  readDnaFile<bitset_size>(read.data(), read_lengths.data(), rg);

  if (rg.numreads > 0) {
    SPRING_LOG_INFO("Constructing dictionaries");
    constructdictionary<bitset_size>(read.data(), dict.data(),
                                     read_lengths.data(), rg.numdict,
                                     rg.numreads, 2, rg.basedir, rg.num_thr);
  }
  SPRING_LOG_INFO("Reordering reads");
  reorder<bitset_size>(read.data(), dict.data(), read_lengths.data(), rg);
  SPRING_LOG_INFO("Writing to file");
  writetofile<bitset_size>(read.data(), read_lengths.data(), rg);
  SPRING_LOG_INFO("Done!");
}

} // namespace spring

#endif // SPRING_REORDER_IMPL_H_
