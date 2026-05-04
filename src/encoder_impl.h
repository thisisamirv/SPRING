#ifndef SPRING_ENCODER_IMPL_H_
#define SPRING_ENCODER_IMPL_H_

#include "bitset_dictionary.h"
#include "core_utils.h"
#include "dna_utils.h"
#include "encoder.h"
#include "fs_utils.h"
#include "progress.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <omp.h>
#include <stdexcept>
#include <vector>

namespace spring {

template <size_t bitset_size> struct encoder_global_b {
  std::vector<std::array<std::bitset<bitset_size>, 128>> basemask;
  std::vector<std::bitset<bitset_size> *> basemask_ptrs;
  int max_readlen;
  std::bitset<bitset_size> mask63;
  encoder_global_b(int max_readlen_param)
      : basemask(), max_readlen(max_readlen_param) {
    basemask.resize(static_cast<size_t>(max_readlen_param));
    basemask_ptrs.resize(static_cast<size_t>(max_readlen_param));
    for (int i = 0; i < max_readlen_param; i++)
      basemask_ptrs[i] = basemask[static_cast<size_t>(i)].data();
  }
  encoder_global_b(const encoder_global_b &) = delete;
  encoder_global_b &operator=(const encoder_global_b &) = delete;
  ~encoder_global_b() = default;
};

namespace detail {

inline void cleanup_thread_encoder_inputs(const encoder_global &encoder_state,
                                          const int thread_id) {
  safe_remove_file(encoder_state.infile_order + '.' +
                   std::to_string(thread_id));
  safe_remove_file(encoder_state.infile_readlength + '.' +
                   std::to_string(thread_id));
  safe_remove_file(encoder_state.infile_RC + '.' + std::to_string(thread_id));
  safe_remove_file(encoder_state.infile_flag + '.' + std::to_string(thread_id));
  safe_remove_file(encoder_state.infile_pos + '.' + std::to_string(thread_id));
  safe_remove_file(encoder_state.infile + '.' + std::to_string(thread_id));
}

template <typename T>
inline bool read_buffer_value(const std::string &buffer, size_t &offset,
                              T &value) {
  if (offset + sizeof(T) > buffer.size()) {
    return false;
  }
  std::memcpy(&value, buffer.data() + offset, sizeof(T));
  offset += sizeof(T);
  return true;
}

inline bool read_buffer_record(const std::string &buffer, size_t &offset,
                               std::string &value) {
  uint32_t record_size = 0;
  if (!read_buffer_value(buffer, offset, record_size)) {
    return false;
  }
  if (offset + record_size > buffer.size()) {
    return false;
  }
  value.assign(buffer.data() + offset, record_size);
  offset += record_size;
  return true;
}

template <size_t bitset_size>
uint32_t write_unaligned_range(
    reordered_stream_artifact &artifact, const std::bitset<bitset_size> *reads,
    const uint32_t *read_orders, const uint16_t *read_lengths,
    const bool *remaining_reads,
    const encoder_global_b<bitset_size> &encoder_bits,
    const uint32_t begin_read_index, const uint32_t end_read_index,
    uint64_t &unaligned_length, const encoder_global &eg) {
  uint32_t aligned_read_count = 0;
  for (uint32_t read_index = begin_read_index; read_index < end_read_index;
       read_index++) {
    if (!remaining_reads[read_index])
      continue;

    aligned_read_count++;
    const std::string unaligned_read = bitsettostring<bitset_size>(
        reads[read_index], read_lengths[read_index], encoder_bits);
    artifact.read_order_entries.push_back(read_orders[read_index]);
    artifact.read_length_entries.push_back(read_lengths[read_index]);
    const uint32_t unaligned_size =
        static_cast<uint32_t>(unaligned_read.size());
    const size_t old_size = artifact.unaligned_serialized.size();
    artifact.unaligned_serialized.resize(old_size + sizeof(uint32_t) +
                                         unaligned_read.size());
    std::memcpy(artifact.unaligned_serialized.data() + old_size,
                &unaligned_size, sizeof(uint32_t));
    if (!unaligned_read.empty()) {
      std::memcpy(artifact.unaligned_serialized.data() + old_size +
                      sizeof(uint32_t),
                  unaligned_read.data(), unaligned_read.size());
    }
    unaligned_length += read_lengths[read_index];
  }

  return aligned_read_count;
}

} // namespace detail

inline void
initialize_encoder_dict_ranges(std::array<bbhashdict, NUM_DICT_ENCODER> &dict,
                               const int max_readlen) {
  if (max_readlen > 50) {
    dict[0].start = 0;
    dict[0].end = 20;
    dict[1].start = 21;
    dict[1].end = 41;
    return;
  }

  dict[0].start = 0;
  dict[0].end = 20 * max_readlen / 50;
  dict[1].start = 20 * max_readlen / 50 + 1;
  dict[1].end = 41 * max_readlen / 50;
}

template <size_t bitset_size>
std::string bitsettostring(std::bitset<bitset_size> encoded_bases,
                           const uint16_t readlen,
                           const encoder_global_b<bitset_size> &egb) {
  static const char reverse_base_lookup[8] = {'A', 'N', 'G', 0, 'C', 0, 'T', 0};
  std::string read_string;
  read_string.resize(readlen);
  unsigned long long packed_bases;
  for (int block_index = 0; block_index < 3 * readlen / 63 + 1; block_index++) {
    packed_bases = (encoded_bases & egb.mask63).to_ullong();
    encoded_bases >>= 63;
    for (int read_index = 21 * block_index;
         read_index < 21 * block_index + 21 && read_index < readlen;
         read_index++) {
      read_string[read_index] = reverse_base_lookup[packed_bases % 8];
      packed_bases /= 8;
    }
  }
  return read_string;
}

template <size_t bitset_size>
void encode(std::bitset<bitset_size> *reads, bbhashdict *dictionaries,
            uint32_t *read_orders, uint16_t *read_lengths,
            bool *remaining_reads, OmpLock *read_locks,
            OmpLock *dictionary_locks,
            std::vector<encoded_metadata_buffer> &thread_metadata_outputs,
            const reorder_encoder_artifact &reorder_artifact,
            const encoder_global &eg,
            const encoder_global_b<bitset_size> &egb) {
  static const int thresh_s = THRESH_ENCODER;
  static const int maxsearch = MAX_SEARCH_ENCODER;

  std::vector<std::bitset<bitset_size>> index_masks(
      static_cast<size_t>(eg.numdict_s));
  generateindexmasks<bitset_size>(index_masks.data(), dictionaries,
                                  eg.numdict_s, 3);

  std::vector<std::vector<std::bitset<bitset_size>>> length_masks;
  length_masks.assign(static_cast<size_t>(eg.max_readlen) + 1,
                      std::vector<std::bitset<bitset_size>>(
                          static_cast<size_t>(eg.max_readlen) + 1));
  std::vector<std::bitset<bitset_size> *> length_masks_ptrs;
  length_masks_ptrs.reserve(static_cast<size_t>(eg.max_readlen) + 1);
  for (int i = 0; i <= eg.max_readlen; ++i)
    length_masks_ptrs.push_back(length_masks[static_cast<size_t>(i)].data());
  generatemasks<bitset_size>(length_masks_ptrs.data(), eg.max_readlen, 3);

  SPRING_LOG_INFO("Encoding reads");
  SPRING_LOG_DEBUG("block_id=enc-main, Encoder start: numreads=" +
                   std::to_string(eg.numreads) + ", singleton_pool=" +
                   std::to_string(eg.numreads_s + eg.numreads_N) +
                   ", threads=" + std::to_string(eg.num_thr) +
                   ", num_dict=" + std::to_string(eg.numdict_s));
  std::vector<std::string> open_stream_errors(static_cast<size_t>(eg.num_thr));
#pragma omp parallel
  {
    bool done = false;
    int thread_id = omp_get_thread_num();
    const std::string block_id =
        std::string("enc-thread-") + std::to_string(thread_id);
    encoded_metadata_buffer thread_metadata;
    if (static_cast<size_t>(thread_id) >=
        reorder_artifact.aligned_shards.size()) {
      open_stream_errors[static_cast<size_t>(thread_id)] =
          std::string("Thread ") + std::to_string(thread_id) +
          ": Missing in-memory reorder shard.";
      done = true;
    }
    const reorder_encoder_shard *input_shard =
        done ? nullptr
             : &reorder_artifact.aligned_shards[static_cast<size_t>(thread_id)];
    size_t read_cursor = 0;
    size_t flag_cursor = 0;
    size_t position_cursor = 0;
    size_t order_cursor = 0;
    size_t orientation_cursor = 0;
    size_t read_length_cursor = 0;
    int64_t bucket_range[2];
    uint64_t bucket_start_index;
    uint64_t lookup_key;
    uint64_t abs_pos = 0;
    bool matched_read = 0;
    std::string current_read, reference_contig;
    std::bitset<bitset_size> forward_bitset, reverse_bitset, masked_window_bits;
    char read_flag = '0', orientation = 'd';
    std::list<contig_reads> current_contig;
    int64_t relative_position = 0;
    uint16_t read_length;
    uint32_t read_order, contig_read_count = 0;
    std::array<std::list<uint32_t>, NUM_DICT_ENCODER> deleted_rids;
    uint64_t thread_contig_flush_count = 0;
    uint64_t thread_forced_break_count = 0;
    uint64_t thread_singleton_absorbed = 0;

    static std::atomic<uint32_t> total_reads_encoded{0};
    while (!done) {
      if (flag_cursor >= input_shard->flag_bytes.size()) {
        done = true;
      } else {
        read_flag = input_shard->flag_bytes[flag_cursor++];
      }
      if (!done) {
        contig_read_count++;
        if (!detail::read_buffer_record(input_shard->read_bytes, read_cursor,
                                        current_read)) {
          throw std::runtime_error(
              "Failed to read reordered read shard at read count " +
              std::to_string(contig_read_count));
        }
        if (orientation_cursor >= input_shard->orientation_bytes.size()) {
          throw std::runtime_error(
              "Failed to read orientation from in-memory shard at read count " +
              std::to_string(contig_read_count));
        }
        orientation = input_shard->orientation_bytes[orientation_cursor++];
        if (!detail::read_buffer_value(input_shard->position_bytes,
                                       position_cursor, relative_position)) {
          throw std::runtime_error(
              "Failed to read position from in-memory shard at read count " +
              std::to_string(contig_read_count));
        }
        if (!detail::read_buffer_value(input_shard->order_bytes, order_cursor,
                                       read_order)) {
          throw std::runtime_error(
              "Failed to read order from in-memory shard at read count " +
              std::to_string(contig_read_count));
        }
        if (detail::read_buffer_value(input_shard->read_length_bytes,
                                      read_length_cursor, read_length)) {
          uint32_t total = ++total_reads_encoded;
          if (total % 100000 == 0) {
            if (auto *progress = ProgressBar::GlobalInstance()) {
              progress->update(static_cast<float>(total) / eg.numreads);
            }
          }
        } else {
          throw std::runtime_error(
              "Failed to read length from in-memory shard at read count " +
              std::to_string(contig_read_count));
        }
      }
      // Safety check: force a contig break if the relative position is
      // excessively large, which could lead to massive memory allocation for
      // the consensus.
      bool excessive_growth = (relative_position > MAX_CONTIG_GROWTH);

      if (read_flag == '0' || done || contig_read_count > 10000000 ||
          excessive_growth) {
        if (!done)
          thread_contig_flush_count++;
        if (excessive_growth && !done && read_flag != '0') {
          thread_forced_break_count++;
          std::cerr << "Warning: Forcing contig break due to excessive growth ("
                    << relative_position << " > " << MAX_CONTIG_GROWTH
                    << ") at read count " << contig_read_count << "\n";
        }
        if (!current_contig.empty()) {
          current_contig.sort([](const contig_reads &a, const contig_reads &b) {
            return a.pos < b.pos;
          });
          auto contig_it = current_contig.begin();
          int64_t first_pos = (*contig_it).pos;
          for (; contig_it != current_contig.end(); ++contig_it)
            (*contig_it).pos -= first_pos;

          reference_contig = buildcontig(current_contig, contig_read_count);
          if ((int64_t)reference_contig.size() >= eg.max_readlen &&
              (eg.numreads_s + eg.numreads_N > 0)) {
            // Scan each contig window for singleton reads that can be folded
            // in.
            forward_bitset.reset();
            reverse_bitset.reset();
            stringtobitset(reference_contig.substr(0, eg.max_readlen),
                           eg.max_readlen, forward_bitset,
                           const_cast<std::bitset<bitset_size> **>(
                               egb.basemask_ptrs.data()));
            stringtobitset(
                reverse_complement(reference_contig.substr(0, eg.max_readlen),
                                   eg.max_readlen),
                eg.max_readlen, reverse_bitset,
                const_cast<std::bitset<bitset_size> **>(
                    egb.basemask_ptrs.data()));
            for (long window_start = 0;
                 window_start <
                 (int64_t)reference_contig.size() - eg.max_readlen + 1;
                 window_start++) {
              for (int orientation_index = 0; orientation_index < 2;
                   orientation_index++) {
                for (int dictionary_index = 0; dictionary_index < eg.numdict_s;
                     dictionary_index++) {
                  if (!orientation_index)
                    masked_window_bits =
                        forward_bitset & index_masks[dictionary_index];
                  else
                    masked_window_bits =
                        reverse_bitset & index_masks[dictionary_index];
                  lookup_key = (masked_window_bits >>
                                3 * dictionaries[dictionary_index].start)
                                   .to_ullong();
                  bucket_start_index =
                      (*dictionaries[dictionary_index].bphf)(lookup_key);
                  if (bucket_start_index >=
                      dictionaries[dictionary_index].numkeys)
                    continue;
                  if (!omp_test_lock(dictionary_locks[detail::lock_shard(
                                                          bucket_start_index)]
                                         .get()))
                    continue;
                  dictionaries[dictionary_index].findpos(bucket_range,
                                                         bucket_start_index);
                  if (dictionaries[dictionary_index]
                          .empty_bin[bucket_start_index]) {
                    omp_unset_lock(
                        dictionary_locks[detail::lock_shard(bucket_start_index)]
                            .get());
                    continue;
                  }
                  if (!detail::valid_bucket_range(
                          dictionaries[dictionary_index], bucket_range)) {
                    dictionaries[dictionary_index]
                        .empty_bin[bucket_start_index] = true;
                    omp_unset_lock(
                        dictionary_locks[detail::lock_shard(bucket_start_index)]
                            .get());
                    continue;
                  }
                  uint64_t candidate_key =
                      ((reads[dictionaries[dictionary_index]
                                  .read_id[bucket_range[0]]] &
                        index_masks[dictionary_index]) >>
                       3 * dictionaries[dictionary_index].start)
                          .to_ullong();
                  if (lookup_key == candidate_key) {
                    for (int64_t bucket_index = bucket_range[1] - 1;
                         bucket_index >= bucket_range[0] &&
                         bucket_index >= bucket_range[1] - maxsearch;
                         bucket_index--) {
                      auto read_id =
                          dictionaries[dictionary_index].read_id[bucket_index];
                      int hamming;
                      if (!orientation_index)
                        hamming = ((forward_bitset ^ reads[read_id]) &
                                   length_masks[0][eg.max_readlen -
                                                   read_lengths[read_id]])
                                      .count();
                      else
                        hamming = ((reverse_bitset ^ reads[read_id]) &
                                   length_masks[0][eg.max_readlen -
                                                   read_lengths[read_id]])
                                      .count();
                      if (hamming <= thresh_s) {
                        if (!omp_test_lock(
                                read_locks[detail::lock_shard(read_id)].get()))
                          continue;
                        if (remaining_reads[read_id]) {
                          remaining_reads[read_id] = 0;
                          matched_read = 1;
                        }
                        omp_unset_lock(
                            read_locks[detail::lock_shard(read_id)].get());
                      }
                      if (matched_read == 1) {
                        matched_read = 0;
                        thread_singleton_absorbed++;
                        contig_read_count++;
                        char matched_orientation =
                            orientation_index ? 'r' : 'd';
                        long matched_position =
                            orientation_index ? (window_start + eg.max_readlen -
                                                 read_lengths[read_id])
                                              : window_start;
                        std::string read_string =
                            orientation_index
                                ? reverse_complement(
                                      bitsettostring<bitset_size>(
                                          reads[read_id], read_lengths[read_id],
                                          egb),
                                      read_lengths[read_id])
                                : bitsettostring<bitset_size>(
                                      reads[read_id], read_lengths[read_id],
                                      egb);
                        current_contig.push_back(
                            {read_string, matched_position, matched_orientation,
                             read_orders[read_id], read_lengths[read_id]});
                        for (int delete_dict_index = 0;
                             delete_dict_index < eg.numdict_s;
                             delete_dict_index++) {
                          if (read_lengths[read_id] >
                              dictionaries[delete_dict_index].end)
                            deleted_rids[delete_dict_index].push_back(read_id);
                        }
                      }
                    }
                  }
                  omp_unset_lock(
                      dictionary_locks[detail::lock_shard(bucket_start_index)]
                          .get());
                  for (int delete_dict_index = 0;
                       delete_dict_index < eg.numdict_s; delete_dict_index++)
                    for (auto deleted_it =
                             deleted_rids[delete_dict_index].begin();
                         deleted_it != deleted_rids[delete_dict_index].end();) {
                      masked_window_bits =
                          reads[*deleted_it] & index_masks[delete_dict_index];
                      lookup_key = (masked_window_bits >>
                                    3 * dictionaries[delete_dict_index].start)
                                       .to_ullong();
                      bucket_start_index =
                          (*dictionaries[delete_dict_index].bphf)(lookup_key);
                      if (!omp_test_lock(
                              dictionary_locks[detail::lock_shard(
                                                   bucket_start_index)]
                                  .get())) {
                        ++deleted_it;
                        continue;
                      }
                      dictionaries[delete_dict_index].findpos(
                          bucket_range, bucket_start_index);
                      // Remove matched singletons once they have been absorbed.
                      dictionaries[delete_dict_index].remove(
                          bucket_range, bucket_start_index, *deleted_it);
                      deleted_it =
                          deleted_rids[delete_dict_index].erase(deleted_it);
                      omp_unset_lock(dictionary_locks[detail::lock_shard(
                                                          bucket_start_index)]
                                         .get());
                    }
                }
              }
              if (window_start !=
                  (int64_t)reference_contig.size() - eg.max_readlen) {
                forward_bitset >>= 3;
                forward_bitset = forward_bitset & length_masks[0][0];
                forward_bitset |= egb.basemask[eg.max_readlen - 1][(
                    uint8_t)reference_contig[window_start + eg.max_readlen]];
                reverse_bitset <<= 3;
                reverse_bitset = reverse_bitset & length_masks[0][0];
                reverse_bitset |= egb.basemask[0][(uint8_t)chartorevchar[(
                    uint8_t)reference_contig[window_start + eg.max_readlen]]];
              }
            }
          }
          current_contig.sort([](const contig_reads &a, const contig_reads &b) {
            return a.pos < b.pos;
          });
          writecontig(reference_contig, current_contig, thread_metadata, eg,
                      abs_pos);
        }
        if (!done) {
          current_contig = {{current_read, relative_position, orientation,
                             read_order, read_length}};
          contig_read_count = 1;
        }
      } else if (read_flag == '1') // read found during rightward search
      {
        current_contig.push_back({current_read, relative_position, orientation,
                                  read_order, read_length});
        contig_read_count++;
      }
    }
    thread_metadata_outputs[static_cast<size_t>(thread_id)] =
        std::move(thread_metadata);
    SPRING_LOG_DEBUG(
        "block_id=" + block_id + ", Encoder thread " +
        std::to_string(thread_id) + " summary: contig_flushes=" +
        std::to_string(thread_contig_flush_count) +
        ", forced_breaks=" + std::to_string(thread_forced_break_count) +
        ", absorbed_singletons=" + std::to_string(thread_singleton_absorbed));
  }

  for (const std::string &error_msg : open_stream_errors) {
    if (!error_msg.empty()) {
      std::cerr << error_msg << std::endl;
    }
  }

  // length_masks and index_masks are RAII-managed and freed automatically
}

template <size_t bitset_size>
void setglobalarrays(encoder_global &eg, encoder_global_b<bitset_size> &egb) {
  for (int i = 0; i < 63; i++)
    egb.mask63[i] = 1;
  for (int i = 0; i < eg.max_readlen; i++) {
    egb.basemask[i][(uint8_t)'A'][3 * i] = 0;
    egb.basemask[i][(uint8_t)'A'][3 * i + 1] = 0;
    egb.basemask[i][(uint8_t)'A'][3 * i + 2] = 0;
    egb.basemask[i][(uint8_t)'C'][3 * i] = 0;
    egb.basemask[i][(uint8_t)'C'][3 * i + 1] = 0;
    egb.basemask[i][(uint8_t)'C'][3 * i + 2] = 1;
    egb.basemask[i][(uint8_t)'G'][3 * i] = 0;
    egb.basemask[i][(uint8_t)'G'][3 * i + 1] = 1;
    egb.basemask[i][(uint8_t)'G'][3 * i + 2] = 1;
    egb.basemask[i][(uint8_t)'G'][3 * i + 1] = 1;
    egb.basemask[i][(uint8_t)'G'][3 * i + 2] = 0;
    egb.basemask[i][(uint8_t)'T'][3 * i] = 0;
    egb.basemask[i][(uint8_t)'T'][3 * i + 1] = 1;
    egb.basemask[i][(uint8_t)'T'][3 * i + 2] = 1;
    egb.basemask[i][(uint8_t)'N'][3 * i] = 1;
    egb.basemask[i][(uint8_t)'N'][3 * i + 1] = 0;
    egb.basemask[i][(uint8_t)'N'][3 * i + 2] = 0;
  }

  // enc_noise uses substitution statistics from Minoche et al.
  eg.enc_noise[(uint8_t)'A'][(uint8_t)'C'] = '0';
  eg.enc_noise[(uint8_t)'A'][(uint8_t)'G'] = '1';
  eg.enc_noise[(uint8_t)'A'][(uint8_t)'T'] = '2';
  eg.enc_noise[(uint8_t)'A'][(uint8_t)'N'] = '3';
  eg.enc_noise[(uint8_t)'C'][(uint8_t)'A'] = '0';
  eg.enc_noise[(uint8_t)'C'][(uint8_t)'G'] = '1';
  eg.enc_noise[(uint8_t)'C'][(uint8_t)'T'] = '2';
  eg.enc_noise[(uint8_t)'C'][(uint8_t)'N'] = '3';
  eg.enc_noise[(uint8_t)'G'][(uint8_t)'T'] = '0';
  eg.enc_noise[(uint8_t)'G'][(uint8_t)'A'] = '1';
  eg.enc_noise[(uint8_t)'G'][(uint8_t)'C'] = '2';
  eg.enc_noise[(uint8_t)'G'][(uint8_t)'N'] = '3';
  eg.enc_noise[(uint8_t)'T'][(uint8_t)'G'] = '0';
  eg.enc_noise[(uint8_t)'T'][(uint8_t)'C'] = '1';
  eg.enc_noise[(uint8_t)'T'][(uint8_t)'A'] = '2';
  eg.enc_noise[(uint8_t)'T'][(uint8_t)'N'] = '3';
  eg.enc_noise[(uint8_t)'N'][(uint8_t)'A'] = '0';
  eg.enc_noise[(uint8_t)'N'][(uint8_t)'G'] = '1';
  eg.enc_noise[(uint8_t)'N'][(uint8_t)'C'] = '2';
  eg.enc_noise[(uint8_t)'N'][(uint8_t)'T'] = '3';
  return;
}

template <size_t bitset_size>
void readsingletons(std::bitset<bitset_size> *read, uint32_t *order_s,
                    uint16_t *read_lengths_s,
                    const reorder_encoder_artifact &reorder_artifact,
                    const encoder_global &eg,
                    const encoder_global_b<bitset_size> &egb) {
  std::string s;
  s.reserve(static_cast<size_t>(eg.max_readlen));
  auto **const basemask_ptrs =
      const_cast<std::bitset<bitset_size> **>(egb.basemask_ptrs.data());
  size_t singleton_cursor = 0;
  for (uint32_t i = 0; i < eg.numreads_s; i++) {
    if (!detail::read_buffer_record(reorder_artifact.singleton_read_bytes,
                                    singleton_cursor, s)) {
      throw std::runtime_error(
          "Failed reading singleton read from in-memory reorder artifact.");
    }
    read_lengths_s[i] = static_cast<uint16_t>(s.size());
    stringtobitset<bitset_size>(s, read_lengths_s[i], read[i], basemask_ptrs);
  }
  std::ifstream f(eg.infile_N, std::ios::binary);
  std::vector<char> singleton_n_io_buffer(1 << 20);
  f.rdbuf()->pubsetbuf(singleton_n_io_buffer.data(),
                       singleton_n_io_buffer.size());
  static constexpr std::array<char, 16> int_to_dna_n = {
      'A', 'G', 'C', 'T', 'N', 'N', 'N', 'N',
      'N', 'N', 'N', 'N', 'N', 'N', 'N', 'N'};
  uint8_t encoded_read_bytes[256];
  for (uint32_t i = eg.numreads_s; i < eg.numreads_s + eg.numreads_N; i++) {
    uint16_t readlen = 0;
    if (!f.read(byte_ptr(&readlen), sizeof(uint16_t))) {
      throw std::runtime_error("Failed reading readlen from DNA+N stream.");
    }

    const uint16_t encoded_byte_count =
        static_cast<uint16_t>((readlen + 1U) / 2U);
    if (encoded_byte_count > sizeof(encoded_read_bytes)) {
      throw std::runtime_error(
          "Corrupted DNA+N stream: record length exceeds decoder buffer.");
    }
    if (!f.read(byte_ptr(encoded_read_bytes), encoded_byte_count)) {
      throw std::runtime_error(
          "Failed reading encoded DNA+N payload from stream.");
    }

    s.resize(readlen);
    for (uint16_t base_index = 0; base_index < readlen; ++base_index) {
      const uint8_t packed = encoded_read_bytes[base_index / 2];
      const uint8_t code =
          (base_index % 2 == 0) ? (packed & 0x0F) : (packed >> 4);
      s[base_index] = int_to_dna_n[code & 0x0F];
    }

    read_lengths_s[i] = readlen;
    stringtobitset<bitset_size>(s, readlen, read[i], basemask_ptrs);
  }
  size_t singleton_order_cursor = 0;
  for (uint32_t i = 0; i < eg.numreads_s; i++) {
    if (!detail::read_buffer_value(reorder_artifact.singleton_order_bytes,
                                   singleton_order_cursor, order_s[i])) {
      throw std::runtime_error(
          "Failed reading singleton order from in-memory reorder artifact.");
    }
  }
  std::ifstream f_order_N(eg.infile_order_N, std::ios::binary);
  for (uint32_t i = eg.numreads_s; i < eg.numreads_s + eg.numreads_N; i++)
    f_order_N.read(byte_ptr(&order_s[i]), sizeof(uint32_t));
  f_order_N.close();
}

template <size_t bitset_size>
reordered_stream_artifact
encoder_main(const std::string &temp_dir,
             const reorder_encoder_artifact &reorder_artifact,
             compression_params &cp) {
  if (cp.encoding.num_thr >
      static_cast<int>(compression_params::ReadMetadata::kFileLenThrSize)) {
    throw std::runtime_error(
        std::string("Exceeded maximum supported thread count (") +
        std::to_string(compression_params::ReadMetadata::kFileLenThrSize) +
        "). Increase array size in util.h.");
  }
  encoder_global_b<bitset_size> egb(cp.read_info.max_readlen);
  encoder_global eg;

  eg.basedir = temp_dir;
  eg.infile = eg.basedir + "/temp.dna";
  eg.infile_flag = eg.basedir + "/tempflag.txt";
  eg.infile_pos = eg.basedir + "/temppos.txt";
  eg.infile_RC = eg.basedir + "/read_rev.txt";
  eg.infile_readlength = eg.basedir + "/read_lengths.bin";
  eg.infile_order = eg.basedir + "/read_order.bin";
  eg.infile_N = eg.basedir + "/input_N.dna";
  eg.infile_order_N = eg.basedir + "/read_order_N.bin";
  eg.outfile_seq = eg.basedir + "/read_seq.bin";
  eg.outfile_pos = eg.basedir + "/read_pos.bin";
  eg.outfile_noise = eg.basedir + "/read_noise.txt";
  eg.outfile_noisepos = eg.basedir + "/read_noisepos.bin";
  eg.outfile_unaligned = eg.basedir + "/read_unaligned.txt";

  eg.max_readlen = cp.read_info.max_readlen;
  eg.num_thr = cp.encoding.num_thr;

  omp_set_num_threads(eg.num_thr);
  getDataParams(eg, cp, reorder_artifact); // populate numreads
  setglobalarrays<bitset_size>(eg, egb);
  const uint32_t singleton_pool_size = eg.numreads_s + eg.numreads_N;
  eg.numdict_s = (singleton_pool_size < DICT_SINGLE_STAGE_READ_THRESHOLD)
                     ? 1
                     : NUM_DICT_ENCODER;
  SPRING_LOG_DEBUG("Encoder dictionary configuration: active_dicts=" +
                   std::to_string(eg.numdict_s) +
                   ", singleton_pool=" + std::to_string(singleton_pool_size));

  std::vector<std::bitset<bitset_size>> read;
  std::vector<uint32_t> order_s;
  std::vector<uint16_t> read_lengths_s;
  std::vector<encoded_metadata_buffer> thread_metadata_outputs(
      static_cast<size_t>(eg.num_thr));
  read.resize(static_cast<size_t>(singleton_pool_size));
  order_s.resize(static_cast<size_t>(singleton_pool_size));
  read_lengths_s.resize(static_cast<size_t>(singleton_pool_size));
  SPRING_LOG_INFO("Reading singletons...");
  readsingletons<bitset_size>(read.data(), order_s.data(),
                              read_lengths_s.data(), reorder_artifact, eg, egb);
  SPRING_LOG_DEBUG(
      "block_id=enc-main, Singleton read pools loaded: clean_singletons=" +
      std::to_string(eg.numreads_s) +
      ", N_reads=" + std::to_string(eg.numreads_N));

  safe_remove_file(eg.infile_N);
  SPRING_LOG_INFO("Correcting order...");
  reorder_encoder_artifact corrected_reorder_artifact = reorder_artifact;
  correct_order(order_s.data(), eg, corrected_reorder_artifact);

  std::array<bbhashdict, NUM_DICT_ENCODER> dict;
  initialize_encoder_dict_ranges(dict, eg.max_readlen);
  if (singleton_pool_size > 0) {
    SPRING_LOG_INFO("Building encoder dictionary for " +
                    std::to_string(singleton_pool_size) + " singletons...");
    constructdictionary<bitset_size>(
        read.data(), dict.data(), read_lengths_s.data(), eg.numdict_s,
        singleton_pool_size, 3, eg.basedir, eg.num_thr);
  }
  SPRING_LOG_INFO("Starting main encoding loop...");

  const uint32_t num_locks = NUM_LOCKS_REORDER;
  std::vector<OmpLock> read_locks(num_locks);
  std::vector<OmpLock> dictionary_locks(num_locks);
  auto remaining_reads_storage = std::make_unique<bool[]>(
      static_cast<size_t>(eg.numreads_s) + eg.numreads_N);
  bool *remaining_reads = remaining_reads_storage.get();
  std::fill(remaining_reads, remaining_reads + eg.numreads_s + eg.numreads_N,
            true);

  encode<bitset_size>(read.data(), dict.data(), order_s.data(),
                      read_lengths_s.data(), remaining_reads, read_locks.data(),
                      dictionary_locks.data(), thread_metadata_outputs,
                      corrected_reorder_artifact, eg, egb);

  reordered_stream_artifact artifact;

  // Cleanup state arrays and locks (OmpLock destructors handle locks)
  // remaining_reads_storage will be freed automatically

  // Final sequence block synchronization.
  // We calculate absolute offsets for global positioning, then pack each
  // thread's sequence data into a separate .bsc block for the decompressor.
  std::vector<uint64_t> file_len_seq_thr(static_cast<size_t>(eg.num_thr));
  std::vector<uint64_t> thread_sequence_bases(static_cast<size_t>(eg.num_thr),
                                              0);
  uint64_t abs_pos = 0;
  for (int tid = 0; tid < eg.num_thr; tid++) {
    file_len_seq_thr[static_cast<size_t>(tid)] =
        thread_metadata_outputs[static_cast<size_t>(tid)].sequence_base_count;
  }

  for (int tid = 0; tid < eg.num_thr; tid++) {
    thread_sequence_bases[static_cast<size_t>(tid)] = abs_pos;
    abs_pos += file_len_seq_thr[static_cast<size_t>(tid)];
  }

  for (int tid = 0; tid < eg.num_thr; tid++) {
    const encoded_metadata_buffer &thread_output =
        thread_metadata_outputs[static_cast<size_t>(tid)];
    const uint64_t thread_base =
        thread_sequence_bases[static_cast<size_t>(tid)];
    for (const uint64_t position : thread_output.position_entries) {
      artifact.position_entries.push_back(position + thread_base);
    }
    artifact.noise_serialized.insert(artifact.noise_serialized.end(),
                                     thread_output.noise_serialized.begin(),
                                     thread_output.noise_serialized.end());
    artifact.noise_positions.insert(artifact.noise_positions.end(),
                                    thread_output.noise_positions.begin(),
                                    thread_output.noise_positions.end());
    artifact.orientation_entries.insert(
        artifact.orientation_entries.end(),
        thread_output.orientation_entries.begin(),
        thread_output.orientation_entries.end());
    artifact.read_length_entries.insert(
        artifact.read_length_entries.end(),
        thread_output.read_length_entries.begin(),
        thread_output.read_length_entries.end());
    artifact.read_order_entries.insert(artifact.read_order_entries.end(),
                                       thread_output.read_order_entries.begin(),
                                       thread_output.read_order_entries.end());
  }

  uint64_t len_unaligned = 0;

  const uint32_t remaining_singleton_reads = detail::write_unaligned_range(
      artifact, read.data(), order_s.data(), read_lengths_s.data(),
      remaining_reads, egb, 0, eg.numreads_s, len_unaligned, eg);
  const uint32_t remaining_n_reads = detail::write_unaligned_range(
      artifact, read.data(), order_s.data(), read_lengths_s.data(),
      remaining_reads, egb, eg.numreads_s, eg.numreads_s + eg.numreads_N,
      len_unaligned, eg);
  artifact.unaligned_char_count = len_unaligned;
  SPRING_LOG_DEBUG(
      "block_id=enc-main, Encoder residual unaligned writes: singleton_reads=" +
      std::to_string(remaining_singleton_reads) +
      ", N_reads=" + std::to_string(remaining_n_reads) +
      ", unaligned_bases=" + std::to_string(len_unaligned));
  for (int tid = 0; tid < cp.encoding.num_thr; tid++) {
    if (static_cast<size_t>(tid) >=
        compression_params::ReadMetadata::kFileLenThrSize) {
      throw std::runtime_error(
          std::string("Exceeded maximum supported thread count (") +
          std::to_string(compression_params::ReadMetadata::kFileLenThrSize) +
          "). Increase array size in params.h.");
    }
    cp.read_info.file_len_seq_thr[tid] = file_len_seq_thr[tid];
  }

  uint64_t total_seq_bases = 0;
  for (int tid = 0; tid < cp.encoding.num_thr; tid++) {
    total_seq_bases += file_len_seq_thr[tid];
  }
  SPRING_LOG_DEBUG(
      "block_id=enc-main, Encoder sequence packing summary: threads=" +
      std::to_string(cp.encoding.num_thr) +
      ", total_seq_bases=" + std::to_string(total_seq_bases));

  if (total_seq_bases == 0) {
    SPRING_LOG_DEBUG("block_id=enc-main, Skipping sequence packing: no "
                     "sequence bases to compress.");
    std::ofstream empty_seq_output(eg.outfile_seq + ".bsc",
                                   std::ios::binary | std::ios::trunc);
    if (!empty_seq_output.is_open()) {
      throw std::runtime_error("Failed to create empty sequence archive: " +
                               eg.outfile_seq + ".bsc");
    }
    empty_seq_output.close();
    return artifact;
  }

  // Generate per-thread .bsc sequence blocks as expected by the decompressor.
  pack_compress_seq(eg, thread_metadata_outputs, file_len_seq_thr.data());

  // Update metadata with final exact base counts after packing.
  for (int tid = 0; tid < cp.encoding.num_thr; tid++) {
    cp.read_info.file_len_seq_thr[tid] = file_len_seq_thr[tid];
  }

  // read/order_s/read_lengths_s are RAII-managed (std::vector)
  return artifact;
}

} // namespace spring

#endif // SPRING_ENCODER_IMPL_H_
