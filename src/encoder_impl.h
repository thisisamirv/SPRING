#ifndef SPRING_ENCODER_IMPL_H_
#define SPRING_ENCODER_IMPL_H_

#include "bitset_dictionary.h"
#include "encoder.h"
#include "progress.h"
#include "util.h"
#include <algorithm>
#include <array>
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
  std::bitset<bitset_size> **basemask;
  int max_readlen;
  std::bitset<bitset_size> mask63;
  encoder_global_b(int max_readlen_param)
      : basemask(nullptr), max_readlen(max_readlen_param) {
    basemask = new std::bitset<bitset_size> *[max_readlen_param];
    for (int i = 0; i < max_readlen_param; i++)
      basemask[i] = new std::bitset<bitset_size>[128];
  }
  encoder_global_b(const encoder_global_b &) = delete;
  encoder_global_b &operator=(const encoder_global_b &) = delete;
  ~encoder_global_b() {
    for (int i = 0; i < max_readlen; i++)
      delete[] basemask[i];
    delete[] basemask;
  }
};

namespace detail {

inline std::string thread_output_tmp_path(const std::string &base_path,
                                          const int thread_id) {
  return base_path + '.' + std::to_string(thread_id) + ".tmp";
}

inline void append_thread_stream(
    std::ofstream &merged_output, const std::string &thread_output_path,
    const std::ios::openmode mode = std::ios::in | std::ios::binary) {
  std::ifstream thread_input(thread_output_path, mode);
  if (!thread_input.is_open()) {
    std::cerr << "Warning: Failed to open thread output stream for merging at "
              << thread_output_path << "\n";
    return;
  }
  const size_t buffer_size = 1024 * 1024; // 1MB buffer
  std::unique_ptr<char[]> buffer(new char[buffer_size]);
  while (thread_input.read(buffer.get(), buffer_size)) {
    merged_output.write(buffer.get(), thread_input.gcount());
  }
  if (thread_input.gcount() > 0) {
    merged_output.write(buffer.get(), thread_input.gcount());
  }
  merged_output.clear();
}

inline void cleanup_thread_encode_files(const encoder_global &encoder_state,
                                        const int thread_id) {
  remove(
      (encoder_state.infile_order + '.' + std::to_string(thread_id)).c_str());
  remove(thread_output_tmp_path(encoder_state.infile_order, thread_id).c_str());
  remove((encoder_state.infile_readlength + '.' + std::to_string(thread_id))
             .c_str());
  remove(thread_output_tmp_path(encoder_state.infile_readlength, thread_id)
             .c_str());
  remove((encoder_state.outfile_noisepos + '.' + std::to_string(thread_id))
             .c_str());
  remove(
      (encoder_state.outfile_noise + '.' + std::to_string(thread_id)).c_str());
  remove(thread_output_tmp_path(encoder_state.infile_RC, thread_id).c_str());
  remove((encoder_state.infile_RC + '.' + std::to_string(thread_id)).c_str());
  remove((encoder_state.infile_flag + '.' + std::to_string(thread_id)).c_str());
  remove((encoder_state.infile_pos + '.' + std::to_string(thread_id)).c_str());
  remove((encoder_state.infile + '.' + std::to_string(thread_id)).c_str());
}

inline void merge_thread_encoded_outputs(const encoder_global &encoder_state) {
  std::ofstream order_output(encoder_state.infile_order,
                             std::ios::binary | std::ios::app);
  std::ofstream read_length_output(encoder_state.infile_readlength,
                                   std::ios::binary | std::ios::app);
  std::ofstream noise_position_output(encoder_state.outfile_noisepos,
                                      std::ios::binary | std::ios::app);
  std::ofstream noise_output(encoder_state.outfile_noise,
                             std::ios::binary | std::ios::app);
  std::ofstream orientation_output(encoder_state.infile_RC,
                                   std::ios::binary | std::ios::app);

  for (int thread_id = 0; thread_id < encoder_state.num_thr; thread_id++) {
    append_thread_stream(
        order_output,
        thread_output_tmp_path(encoder_state.infile_order, thread_id),
        std::ios::binary);
    append_thread_stream(
        read_length_output,
        thread_output_tmp_path(encoder_state.infile_readlength, thread_id),
        std::ios::binary);
    append_thread_stream(
        orientation_output,
        thread_output_tmp_path(encoder_state.infile_RC, thread_id));
    append_thread_stream(noise_position_output,
                         encoder_state.outfile_noisepos + '.' +
                             std::to_string(thread_id),
                         std::ios::binary);
    append_thread_stream(noise_output, encoder_state.outfile_noise + '.' +
                                           std::to_string(thread_id));

    cleanup_thread_encode_files(encoder_state, thread_id);
  }
}

template <size_t bitset_size>
uint32_t write_unaligned_range(
    std::ofstream &order_output, std::ofstream &read_length_output,
    std::ofstream &unaligned_output, const std::bitset<bitset_size> *reads,
    const uint32_t *read_orders, const uint16_t *read_lengths,
    const bool *remaining_reads,
    const encoder_global_b<bitset_size> &encoder_bits,
    const uint32_t begin_read_index, const uint32_t end_read_index,
    uint64_t &unaligned_length) {
  uint32_t aligned_read_count = 0;
  for (uint32_t read_index = begin_read_index; read_index < end_read_index;
       read_index++) {
    if (!remaining_reads[read_index])
      continue;

    aligned_read_count++;
    const std::string unaligned_read = bitsettostring<bitset_size>(
        reads[read_index], read_lengths[read_index], encoder_bits);
    write_dnaN_in_bits(unaligned_read, unaligned_output);
    order_output.write(byte_ptr(&read_orders[read_index]), sizeof(uint32_t));
    read_length_output.write(byte_ptr(&read_lengths[read_index]),
                             sizeof(uint16_t));
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
            const std::vector<uint16_t> &all_read_lengths,
            bool *remaining_reads, omp_lock_t *read_locks,
            omp_lock_t *dictionary_locks, const encoder_global &eg,
            const encoder_global_b<bitset_size> &egb) {
  static const int thresh_s = THRESH_ENCODER;
  static const int maxsearch = MAX_SEARCH_ENCODER;
  // Use a fixed large size for locks to avoid any BBHash-related out-of-bounds
  // indexing.
  const uint32_t num_locks = NUM_LOCKS_REORDER;

  std::bitset<bitset_size> *index_masks =
      new std::bitset<bitset_size>[eg.numdict_s];
  generateindexmasks<bitset_size>(index_masks, dictionaries, eg.numdict_s, 3);
  std::bitset<bitset_size> **length_masks =
      new std::bitset<bitset_size> *[eg.max_readlen + 1];
  for (int i = 0; i <= eg.max_readlen; i++)
    length_masks[i] = new std::bitset<bitset_size>[eg.max_readlen + 1];
  generatemasks<bitset_size>(length_masks, eg.max_readlen, 3);

  Logger::log_info("Encoding reads");
#pragma omp parallel
  {
    bool done = false;
    int thread_id = omp_get_thread_num();
    std::ifstream read_input(eg.infile + '.' + std::to_string(thread_id),
                             std::ios::binary);

    std::ifstream flag_stream(eg.infile_flag + '.' + std::to_string(thread_id),
                              std::ios::binary);
    std::ifstream position_stream(
        eg.infile_pos + '.' + std::to_string(thread_id), std::ios::binary);
    std::ifstream order_input(eg.infile_order + '.' + std::to_string(thread_id),
                              std::ios::binary);
    std::ifstream orientation_stream(
        eg.infile_RC + '.' + std::to_string(thread_id), std::ios::binary);
    std::ifstream read_length_stream(eg.infile_readlength + '.' +
                                         std::to_string(thread_id),
                                     std::ios::binary);
    std::ofstream sequence_output(
        eg.outfile_seq + '.' + std::to_string(thread_id), std::ios::binary);
    std::ofstream position_output(
        eg.outfile_pos + '.' + std::to_string(thread_id), std::ios::binary);
    std::ofstream noise_output(
        eg.outfile_noise + '.' + std::to_string(thread_id), std::ios::binary);
    std::ofstream noise_position_output(eg.outfile_noisepos + '.' +
                                            std::to_string(thread_id),
                                        std::ios::binary);
    std::ofstream order_output(eg.infile_order + '.' +
                                   std::to_string(thread_id) + ".tmp",
                               std::ios::binary);
    std::ofstream orientation_output(eg.infile_RC + '.' +
                                         std::to_string(thread_id) + ".tmp",
                                     std::ios::binary);
    std::ofstream read_length_output(eg.infile_readlength + '.' +
                                         std::to_string(thread_id) + ".tmp",
                                     std::ios::binary);
    // Check if any streams failed to open.
    if (!read_input.is_open() || !flag_stream.is_open() ||
        !position_stream.is_open() || !order_input.is_open() ||
        !orientation_stream.is_open() || !read_length_stream.is_open() ||
        !sequence_output.is_open() || !position_output.is_open() ||
        !noise_output.is_open() || !noise_position_output.is_open() ||
        !order_output.is_open() || !read_length_output.is_open() ||
        !orientation_output.is_open()) {
      std::string error_msg = "Thread " + std::to_string(thread_id) +
                              ": Failed to open one or more temporary "
                              "files in encoder. Working directory: " +
                              eg.basedir;
#pragma omp critical
      {
        std::cerr << error_msg << std::endl;
      }
      done = true;
    }
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

    static std::atomic<uint32_t> total_reads_encoded{0};
    while (!done) {
      if (!flag_stream.get(read_flag)) {
        done = true;
      }
      if (!done) {
        contig_read_count++;
        read_dna_from_bits(current_read, read_input);
        if (!orientation_stream.get(orientation)) {
          throw std::runtime_error(
              "Failed to read orientation from stream at read count " +
              std::to_string(contig_read_count));
        }
        if (!position_stream.read(byte_ptr(&relative_position),
                                  sizeof(int64_t))) {
          throw std::runtime_error(
              "Failed to read position from stream at read count " +
              std::to_string(contig_read_count));
        }
        if (!order_input.read(byte_ptr(&read_order), sizeof(uint32_t))) {
          throw std::runtime_error(
              "Failed to read order from stream at read count " +
              std::to_string(contig_read_count));
        }
        if (read_length_stream.read(byte_ptr(&read_length), sizeof(uint16_t))) {
          uint32_t total = ++total_reads_encoded;
          if (total % 100000 == 0) {
            if (auto *progress = ProgressBar::GlobalInstance()) {
              progress->update(static_cast<float>(total) / eg.numreads);
            }
          }
        } else {
          throw std::runtime_error(
              "Failed to read length from stream at read count " +
              std::to_string(contig_read_count));
        }
      }
      // Safety check: force a contig break if the relative position is
      // excessively large, which could lead to massive memory allocation for
      // the consensus.
      bool excessive_growth = (relative_position > MAX_CONTIG_GROWTH);

      if (read_flag == '0' || done || contig_read_count > 10000000 ||
          excessive_growth) {
        if (excessive_growth && !done && read_flag != '0') {
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
                           eg.max_readlen, forward_bitset, egb.basemask);
            stringtobitset(
                reverse_complement(reference_contig.substr(0, eg.max_readlen),
                                   eg.max_readlen),
                eg.max_readlen, reverse_bitset, egb.basemask);
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
                  if (!omp_test_lock(&dictionary_locks[detail::lock_shard(
                          bucket_start_index)]))
                    continue;
                  dictionaries[dictionary_index].findpos(bucket_range,
                                                         bucket_start_index);
                  if (dictionaries[dictionary_index]
                          .empty_bin[bucket_start_index]) {
                    omp_unset_lock(&dictionary_locks[detail::lock_shard(
                        bucket_start_index)]);
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
                                &read_locks[detail::lock_shard(read_id)]))
                          continue;
                        if (remaining_reads[read_id]) {
                          remaining_reads[read_id] = 0;
                          matched_read = 1;
                        }
                        omp_unset_lock(
                            &read_locks[detail::lock_shard(read_id)]);
                      }
                      if (matched_read == 1) {
                        matched_read = 0;
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
                  omp_unset_lock(&dictionary_locks[detail::lock_shard(
                      bucket_start_index)]);
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
                      if (!omp_test_lock(&dictionary_locks[detail::lock_shard(
                              bucket_start_index)])) {
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
                      omp_unset_lock(&dictionary_locks[detail::lock_shard(
                          bucket_start_index)]);
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
          writecontig(reference_contig, current_contig, sequence_output,
                      position_output, noise_output, noise_position_output,
                      order_output, orientation_output, read_length_output, eg,
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
    read_input.close();
    flag_stream.close();
    position_stream.close();
    order_input.close();
    orientation_stream.close();
    read_length_stream.close();
    sequence_output.close();
    position_output.close();
    noise_output.close();
    noise_position_output.close();
    order_output.close();
    read_length_output.close();
    orientation_output.close();
  }

  for (int read_length_index = 0; read_length_index <= eg.max_readlen;
       read_length_index++)
    delete[] length_masks[read_length_index];
  delete[] length_masks;
  delete[] index_masks;
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
                    uint16_t *read_lengths_s, const encoder_global &eg,
                    const encoder_global_b<bitset_size> &egb) {
  std::ifstream f(eg.infile + ".singleton",
                  std::ifstream::in | std::ios::binary);
  std::string s;
  for (uint32_t i = 0; i < eg.numreads_s; i++) {
    read_dna_from_bits(s, f);
    read_lengths_s[i] = s.length();
    stringtobitset<bitset_size>(s, read_lengths_s[i], read[i], egb.basemask);
  }
  f.close();
  remove((eg.infile + ".singleton").c_str());
  f.open(eg.infile_N, std::ios::binary);
  for (uint32_t i = eg.numreads_s; i < eg.numreads_s + eg.numreads_N; i++) {
    read_dnaN_from_bits(s, f);
    read_lengths_s[i] = s.length();
    stringtobitset<bitset_size>(s, read_lengths_s[i], read[i], egb.basemask);
  }
  std::ifstream f_order_s(eg.infile_order + ".singleton", std::ios::binary);
  for (uint32_t i = 0; i < eg.numreads_s; i++)
    f_order_s.read(byte_ptr(&order_s[i]), sizeof(uint32_t));
  f_order_s.close();
  remove((eg.infile_order + ".singleton").c_str());
  std::ifstream f_order_N(eg.infile_order_N, std::ios::binary);
  for (uint32_t i = eg.numreads_s; i < eg.numreads_s + eg.numreads_N; i++)
    f_order_N.read(byte_ptr(&order_s[i]), sizeof(uint32_t));
  f_order_N.close();
}

template <size_t bitset_size>
void encoder_main(const std::string &temp_dir, compression_params &cp) {
  if (cp.num_thr > static_cast<int>(compression_params::kFileLenThrSize)) {
    throw std::runtime_error(
        std::string("Exceeded maximum supported thread count (") +
        std::to_string(compression_params::kFileLenThrSize) +
        "). Increase array size in util.h.");
  }
  encoder_global_b<bitset_size> egb(cp.max_readlen);
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

  eg.max_readlen = cp.max_readlen;
  eg.num_thr = cp.num_thr;

  omp_set_num_threads(eg.num_thr);
  getDataParams(eg, cp); // populate numreads
  setglobalarrays<bitset_size>(eg, egb);
  const uint32_t singleton_pool_size = eg.numreads_s + eg.numreads_N;
  std::bitset<bitset_size> *read =
      new std::bitset<bitset_size>[singleton_pool_size];
  uint32_t *order_s = new uint32_t[singleton_pool_size];
  uint16_t *read_lengths_s = new uint16_t[singleton_pool_size];
  Logger::log_info("Reading singletons...");
  readsingletons<bitset_size>(read, order_s, read_lengths_s, eg, egb);

  remove(eg.infile_N.c_str());
  Logger::log_info("Correcting order...");
  correct_order(order_s, eg);

  std::array<bbhashdict, NUM_DICT_ENCODER> dict;
  initialize_encoder_dict_ranges(dict, eg.max_readlen);
  if (singleton_pool_size > 0) {
    Logger::log_info("Building encoder dictionary for " +
                     std::to_string(singleton_pool_size) + " singletons...");
    constructdictionary<bitset_size>(read, dict.data(), read_lengths_s,
                                     eg.numdict_s, singleton_pool_size, 3,
                                     eg.basedir, eg.num_thr);
  }
  Logger::log_info("Starting main encoding loop...");

  std::vector<uint16_t> all_read_lengths;
  {
    std::ifstream f_len(eg.infile_readlength, std::ios::binary);
    if (f_len.is_open()) {
      all_read_lengths.resize(cp.num_reads);
      f_len.read(reinterpret_cast<char *>(all_read_lengths.data()),
                 cp.num_reads * sizeof(uint16_t));
    }
  }

  const uint32_t num_locks = NUM_LOCKS_REORDER;
  omp_lock_t *read_locks = new omp_lock_t[num_locks];
  omp_lock_t *dictionary_locks = new omp_lock_t[num_locks];
  bool *remaining_reads = new bool[eg.numreads_s + eg.numreads_N];

  for (uint32_t i = 0; i < num_locks; i++) {
    omp_init_lock(&read_locks[i]);
    omp_init_lock(&dictionary_locks[i]);
  }
  std::fill(remaining_reads, remaining_reads + eg.numreads_s + eg.numreads_N,
            1);

  encode<bitset_size>(read, dict.data(), order_s, read_lengths_s,
                      all_read_lengths, remaining_reads, read_locks,
                      dictionary_locks, eg, egb);

  // Stitch the per-thread streams back into the final encoded outputs.
  detail::merge_thread_encoded_outputs(eg);

  std::ofstream order_output(eg.infile_order,
                             std::ios::out | std::ios::binary | std::ios::app);
  std::ofstream read_length_output(
      eg.infile_readlength, std::ios::out | std::ios::binary | std::ios::app);
  std::ofstream unaligned_output(eg.outfile_unaligned, std::ios::binary);

  uint64_t len_unaligned = 0;

  const uint32_t remaining_singleton_reads = detail::write_unaligned_range(
      order_output, read_length_output, unaligned_output, read, order_s,
      read_lengths_s, remaining_reads, egb, 0, eg.numreads_s, len_unaligned);
  const uint32_t remaining_n_reads = detail::write_unaligned_range(
      order_output, read_length_output, unaligned_output, read, order_s,
      read_lengths_s, remaining_reads, egb, eg.numreads_s,
      eg.numreads_s + eg.numreads_N, len_unaligned);

  order_output.close();
  read_length_output.close();
  unaligned_output.close();

  // Cleanup state arrays and locks
  for (uint32_t i = 0; i < num_locks; i++) {
    omp_destroy_lock(&read_locks[i]);
    omp_destroy_lock(&dictionary_locks[i]);
  }
  delete[] read_locks;
  delete[] dictionary_locks;
  delete[] remaining_reads;

  std::ofstream f_unaligned_count(eg.outfile_unaligned + ".count",
                                  std::ios::out | std::ios::binary);
  f_unaligned_count.write(byte_ptr(&len_unaligned), sizeof(uint64_t));
  f_unaligned_count.close();

  // Final sequence block synchronization.
  // We calculate absolute offsets for global positioning, then pack each
  // thread's sequence data into a separate .bsc block for the decompressor.
  std::vector<uint64_t> file_len_seq_thr(static_cast<size_t>(eg.num_thr));
  uint64_t abs_pos = 0;
  uint64_t abs_pos_thr;

  calculate_sequence_lengths(eg, file_len_seq_thr.data());

  std::ofstream fout_pos(eg.outfile_pos, std::ios::binary);
  for (int tid = 0; tid < eg.num_thr; tid++) {
    std::ifstream fin_pos(eg.outfile_pos + '.' + std::to_string(tid),
                          std::ios::binary);
    while (fin_pos.read(byte_ptr(&abs_pos_thr), sizeof(uint64_t))) {
      abs_pos_thr += abs_pos;
      fout_pos.write(byte_ptr(&abs_pos_thr), sizeof(uint64_t));
    }
    fin_pos.close();
    remove((eg.outfile_pos + '.' + std::to_string(tid)).c_str());
    abs_pos += file_len_seq_thr[tid];
  }
  fout_pos.close();
  for (int tid = 0; tid < cp.num_thr; tid++) {
    if (static_cast<size_t>(tid) >= compression_params::kFileLenThrSize) {
      throw std::runtime_error(
          std::string("Exceeded maximum supported thread count (") +
          std::to_string(compression_params::kFileLenThrSize) +
          "). Increase array size in util.h.");
    }
    cp.file_len_seq_thr[tid] = file_len_seq_thr[tid];
  }

  // Generate per-thread .bsc sequence blocks as expected by the decompressor.
  pack_compress_seq(eg, file_len_seq_thr.data());

  delete[] read;
  delete[] order_s;
  delete[] read_lengths_s;
}

} // namespace spring

#endif // SPRING_ENCODER_IMPL_H_
