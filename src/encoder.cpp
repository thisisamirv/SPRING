// Encodes reordered reads against their consensus/reference representation and
// writes the compressed sequence, position, noise, and unaligned side streams.

#include "encoder.h"
#include "core_utils.h"
#include "fs_utils.h"
#include "libbsc/bsc.h"
#include "progress.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <list>
#include <omp.h>
#include <sstream>
#include <string>
#include <vector>

namespace spring {

struct sequence_pack_paths {
  std::string input_path;
  std::string packed_path;
  std::string tail_path;
  std::string compressed_path;
};

std::string thread_file_path(const std::string &base_path, const int thread_id,
                             const char *suffix = "") {
  return base_path + '.' + std::to_string(thread_id) + suffix;
}

sequence_pack_paths make_sequence_pack_paths(const std::string &base_path,
                                             const int thread_id) {
  sequence_pack_paths paths;
  paths.input_path = thread_file_path(base_path, thread_id);
  paths.packed_path = thread_file_path(base_path, thread_id, ".tmp");
  paths.tail_path = thread_file_path(base_path, thread_id, ".tail");
  paths.compressed_path = thread_file_path(base_path, thread_id, ".bsc");
  return paths;
}

uint64_t write_packed_sequence(const std::string &sequence_path,
                               const std::string &packed_path,
                               const std::string &tail_path,
                               bool bisulfite_ternary) {
  static const std::array<uint8_t, 128> base_to_int = []() {
    std::array<uint8_t, 128> table{};
    table[static_cast<uint8_t>('A')] = 0;
    table[static_cast<uint8_t>('a')] = 0;
    table[static_cast<uint8_t>('C')] = 2;
    table[static_cast<uint8_t>('c')] = 2;
    table[static_cast<uint8_t>('G')] = 1;
    table[static_cast<uint8_t>('g')] = 1;
    table[static_cast<uint8_t>('T')] = 3;
    table[static_cast<uint8_t>('t')] = 3;
    return table;
  }();

  std::ifstream sequence_input(sequence_path, std::ios::binary);
  std::ofstream packed_output(packed_path, std::ios::binary);
  std::ofstream tail_output(tail_path, std::ios::binary);
  if (!sequence_input.is_open()) {
    throw std::runtime_error("Failed to open sequence chunk for packing: " +
                             sequence_path);
  }
  if (!packed_output.is_open()) {
    throw std::runtime_error("Failed to open packed sequence output: " +
                             packed_path);
  }
  if (!tail_output.is_open()) {
    throw std::runtime_error("Failed to open sequence tail output: " +
                             tail_path);
  }
  std::vector<char> packed_buffer;
  packed_buffer.reserve(1 << 16);
  std::array<char, 5> trailing_bases{};
  uint64_t sequence_length = 0;
  size_t trailing_count = 0;
  const size_t bases_per_byte = bisulfite_ternary ? 5 : 4;

  auto flush_packed_buffer = [&]() {
    if (!packed_buffer.empty()) {
      packed_output.write(packed_buffer.data(),
                          static_cast<std::streamsize>(packed_buffer.size()));
      packed_buffer.clear();
    }
  };

  uint32_t record_len;
  while (sequence_input.read(reinterpret_cast<char *>(&record_len),
                             sizeof(uint32_t))) {
    std::string dna(record_len, '\0');
    if (!sequence_input.read(&dna[0], record_len)) {
      throw std::runtime_error("Corrupted sequence chunk: failed to read " +
                               std::to_string(record_len) + " bases");
    }

    sequence_length += record_len;
    for (uint32_t input_index = 0; input_index < record_len; input_index++) {
      trailing_bases[trailing_count++] = dna[input_index];
      if (trailing_count < bases_per_byte) {
        continue;
      }

      if (!bisulfite_ternary) {
        packed_buffer.push_back(
            static_cast<char>(64 * base_to_int[(uint8_t)trailing_bases[3]] +
                              16 * base_to_int[(uint8_t)trailing_bases[2]] +
                              4 * base_to_int[(uint8_t)trailing_bases[1]] +
                              base_to_int[(uint8_t)trailing_bases[0]]));
      } else {
        bool has_c = false;
        for (int k = 0; k < 5; k++) {
          if (trailing_bases[k] == 'C' || trailing_bases[k] == 'c') {
            has_c = true;
            break;
          }
        }
        if (!has_c) {
          uint8_t packed = 0;
          uint8_t p3 = 1;
          for (int k = 0; k < 5; k++) {
            uint8_t val = base_to_int[static_cast<uint8_t>(trailing_bases[k])];
            if (val == 3)
              val = 2; // T is 2 in ternary
            packed += val * p3;
            p3 *= 3;
          }
          packed_buffer.push_back(static_cast<char>(packed));
        } else {
          packed_buffer.push_back(static_cast<char>(243));
          flush_packed_buffer();
          uint16_t escape = 0;
          for (int k = 0; k < 5; k++) {
            uint8_t val = base_to_int[static_cast<uint8_t>(trailing_bases[k])];
            escape |= (val << (2 * k));
          }
          packed_output.write(reinterpret_cast<const char *>(&escape),
                              sizeof(uint16_t));
        }
      }

      if (packed_buffer.size() >= (1 << 15)) {
        flush_packed_buffer();
      }
      trailing_count = 0;
    }

    flush_packed_buffer();
  }

  if (trailing_count > 0) {
    if (!bisulfite_ternary) {
      uint8_t packed_byte = 0;
      for (size_t i = 0; i < trailing_count; ++i) {
        packed_byte |= (base_to_int[(uint8_t)trailing_bases[i]] << (2 * i));
      }
      packed_output.write(reinterpret_cast<const char *>(&packed_byte), 1);
    } else {
      packed_output.put(static_cast<char>(243));
      uint16_t escape = 0;
      for (size_t i = 0; i < trailing_count; ++i) {
        uint8_t val = base_to_int[static_cast<uint8_t>(trailing_bases[i])];
        escape |= (val << (2 * i));
      }
      packed_output.write(reinterpret_cast<const char *>(&escape),
                          sizeof(uint16_t));
    }
  }

  if (!packed_output.good() || !tail_output.good()) {
    throw std::runtime_error("Failed while writing packed sequence chunk: " +
                             packed_path);
  }

  return sequence_length;
}

void pack_sequence_chunk(const encoder_global &encoder_state,
                         const int thread_id,
                         uint64_t *thread_sequence_lengths) {
  const sequence_pack_paths paths =
      make_sequence_pack_paths(encoder_state.outfile_seq, thread_id);
  SPRING_LOG_DEBUG("block_id=enc-pack-chunk-" + std::to_string(thread_id) +
                   ", pack_sequence_chunk start: input=" + paths.input_path +
                   ", packed=" + paths.packed_path);

  const uint64_t sequence_length =
      write_packed_sequence(paths.input_path, paths.packed_path,
                            paths.tail_path, encoder_state.bisulfite_ternary);
  thread_sequence_lengths[thread_id] = sequence_length;
  SPRING_LOG_DEBUG("block_id=enc-pack-chunk-" + std::to_string(thread_id) +
                   ", pack_sequence_chunk done: seq_bases=" +
                   std::to_string(sequence_length));
  safe_remove_file(paths.input_path);
}

void calculate_sequence_lengths(const encoder_global &encoder_state,
                                uint64_t *thread_sequence_lengths) {
  for (int tid = 0; tid < encoder_state.num_thr; tid++) {
    std::string thread_seq_path =
        encoder_state.outfile_seq + '.' + std::to_string(tid);
    std::ifstream seq_in(thread_seq_path, std::ios::binary);
    if (!seq_in.is_open()) {
      thread_sequence_lengths[tid] = 0;
      continue;
    }

    uint64_t total_bases = 0;
    uint32_t record_len;
    while (
        seq_in.read(reinterpret_cast<char *>(&record_len), sizeof(uint32_t))) {
      total_bases += record_len;
      seq_in.seekg(record_len, std::ios::cur);
    }
    thread_sequence_lengths[tid] = total_bases;
    seq_in.close();
  }
}

void pack_compress_seq(const encoder_global &encoder_state,
                       uint64_t *thread_sequence_lengths) {
  if (encoder_state.num_thr <= 0)
    return;
  SPRING_LOG_DEBUG("block_id=enc-pack-main, pack_compress_seq start: threads=" +
                   std::to_string(encoder_state.num_thr));
#pragma omp parallel for schedule(static)
  for (int tid = 0; tid < encoder_state.num_thr; tid++) {
    pack_sequence_chunk(encoder_state, tid, thread_sequence_lengths);
  }
  SPRING_LOG_DEBUG("block_id=enc-pack-main, per-thread packing complete.");

  // Concatenate all thread-local packed files and compress as one monolithic
  // block.
  std::string monolithic_packed_path = encoder_state.outfile_seq + ".packed";
  std::string monolithic_compressed_path = encoder_state.outfile_seq + ".bsc";
  std::ofstream monolithic_out(monolithic_packed_path, std::ios::binary);
  if (!monolithic_out.is_open()) {
    throw std::runtime_error(
        "Failed to open monolithic packed sequence file: " +
        monolithic_packed_path);
  }
  std::vector<char> copy_buffer(1 << 20);
  for (int tid = 0; tid < encoder_state.num_thr; tid++) {
    const sequence_pack_paths paths =
        make_sequence_pack_paths(encoder_state.outfile_seq, tid);
    std::ifstream chunk_in(paths.packed_path, std::ios::binary | std::ios::ate);
    if (chunk_in.is_open()) {
      const uint64_t chunk_size = static_cast<uint64_t>(chunk_in.tellg());
      chunk_in.seekg(0);
      monolithic_out.write(reinterpret_cast<const char *>(&chunk_size),
                           sizeof(uint64_t));
      while (chunk_in.read(copy_buffer.data(), copy_buffer.size())) {
        const std::streamsize bytes = chunk_in.gcount();
        monolithic_out.write(copy_buffer.data(), bytes);
      }
      const std::streamsize tail_bytes = chunk_in.gcount();
      if (tail_bytes > 0) {
        monolithic_out.write(copy_buffer.data(), tail_bytes);
      }
      chunk_in.close();
      safe_remove_file(paths.packed_path);
    } else {
      throw std::runtime_error("Failed to open packed sequence chunk: " +
                               paths.packed_path);
    }
  }
  monolithic_out.close();
  SPRING_LOG_DEBUG(
      "block_id=enc-pack-main, monolithic packed file assembled: path=" +
      monolithic_packed_path);

  {
    std::ifstream packed_size_check(monolithic_packed_path,
                                    std::ios::binary | std::ios::ate);
    if (!packed_size_check.is_open()) {
      throw std::runtime_error(
          "Failed to reopen monolithic packed sequence file: " +
          monolithic_packed_path);
    }
    if (packed_size_check.tellg() <= 0) {
      packed_size_check.close();
      std::ofstream empty_out(monolithic_compressed_path,
                              std::ios::binary | std::ios::trunc);
      if (!empty_out.is_open()) {
        throw std::runtime_error(
            "Failed to create empty compressed sequence file: " +
            monolithic_compressed_path);
      }
      empty_out.close();
      safe_remove_file(monolithic_packed_path);
      return;
    }
    packed_size_check.close();
  }

  SPRING_LOG_DEBUG("block_id=enc-pack-main, invoking BSC_compress: input=" +
                   monolithic_packed_path +
                   ", output=" + monolithic_compressed_path);
  bsc::BSC_compress(monolithic_packed_path.c_str(),
                    monolithic_compressed_path.c_str());
  SPRING_LOG_DEBUG("block_id=enc-pack-main, BSC_compress complete.");
  safe_remove_file(monolithic_packed_path);
}

void rewrite_thread_order_file(
    const std::string &order_path,
    const std::vector<uint32_t> &cumulative_n_reads) {
  const std::string order_tmp_path = order_path + ".tmp";
  std::ifstream order_input(order_path, std::ios::binary);
  std::ofstream order_output(order_tmp_path, std::ios::binary);
  uint32_t read_position;

  while (order_input.read(byte_ptr(&read_position), sizeof(uint32_t))) {
    if (read_position < cumulative_n_reads.size()) {
      read_position += cumulative_n_reads[read_position];
    }
    order_output.write(byte_ptr(&read_position), sizeof(uint32_t));
  }
  order_input.close();
  order_output.close();

  safe_remove_file(order_path);
  safe_rename_file(order_tmp_path, order_path);
}

std::string buildcontig(std::list<contig_reads> &current_contig,
                        const uint32_t &list_size) {
  static const char base_char_lookup[5] = {'A', 'C', 'G', 'T', 'N'};
  auto base_index = [](const uint8_t base) -> uint8_t {
    switch (base) {
    case 'A':
    case 'a':
      return 0;
    case 'C':
    case 'c':
      return 1;
    case 'G':
    case 'g':
      return 2;
    case 'T':
    case 't':
      return 3;
    default:
      return 4;
    }
  };
  if (list_size == 1)
    return (current_contig.front()).read;
  auto current_contig_it = current_contig.begin();
  int64_t current_position = 0;
  int64_t contig_size = 0;
  int64_t positions_to_append;
  std::vector<std::array<uint16_t, 4>> base_counts;
  for (; current_contig_it != current_contig.end(); ++current_contig_it) {
    if (current_contig_it == current_contig.begin())
      positions_to_append = (*current_contig_it).read_length;
    else {
      current_position = (*current_contig_it).pos;
      if (current_position + (*current_contig_it).read_length > contig_size)
        positions_to_append =
            current_position + (*current_contig_it).read_length - contig_size;
      else
        positions_to_append = 0;
    }
    if (contig_size + positions_to_append > MAX_CONTIG_GROWTH) {
      std::stringstream ss;
      ss << "Excessive contig growth detected during encoding: "
         << (contig_size + positions_to_append)
         << " bases (pos=" << current_position
         << ", len=" << (*current_contig_it).read_length
         << ", size=" << contig_size << ") exceeds limit of "
         << MAX_CONTIG_GROWTH
         << ". This indicates either pathological input data or memory "
            "corruption.";
      throw std::runtime_error(ss.str());
    }
    base_counts.insert(base_counts.end(), (size_t)positions_to_append,
                       {0, 0, 0, 0});
    contig_size = contig_size + positions_to_append;
    for (size_t i = 0;
         i < static_cast<size_t>((*current_contig_it).read_length); ++i) {
      const size_t idx = static_cast<size_t>(current_position) + i;
      const uint8_t base_idx =
          base_index(static_cast<uint8_t>((*current_contig_it).read[i]));
      if (base_idx < 4 && base_counts[idx][base_idx] < 65535) {
        base_counts[idx][base_idx] += 1;
      }
    }
  }
  std::string ref(base_counts.size(), 'A');
  for (size_t i = 0; i < base_counts.size(); i++) {
    uint16_t best_base_count = 0;
    uint32_t best_base_index = 0;
    for (uint32_t base_index = 0; base_index < 4; base_index++)
      if (base_counts[i][base_index] > best_base_count) {
        best_base_count = base_counts[i][base_index];
        best_base_index = base_index;
      }
    ref[i] = base_char_lookup[best_base_index];
  }
  return ref;
}

void writecontig(const std::string &ref,
                 std::list<contig_reads> &current_contig, std::ofstream &f_seq,
                 encoded_metadata_buffer &metadata_output,
                 const encoder_global &eg, uint64_t &abs_pos) {
  uint32_t ref_len = static_cast<uint32_t>(ref.size());
  f_seq.write(reinterpret_cast<const char *>(&ref_len), sizeof(uint32_t));
  f_seq << ref;
  uint16_t pos_var;
  size_t previous_noise_offset = 0;
  auto current_contig_it = current_contig.begin();
  long current_position;
  uint64_t absolute_current_position;
  for (; current_contig_it != current_contig.end(); ++current_contig_it) {
    current_position = (*current_contig_it).pos;
    previous_noise_offset = 0;
    for (size_t read_offset = 0;
         read_offset < static_cast<size_t>((*current_contig_it).read_length);
         ++read_offset) {
      const size_t pos = static_cast<size_t>(current_position) + read_offset;
      if ((*current_contig_it).read[read_offset] != ref[pos]) {
        metadata_output.noise_serialized.push_back(eg.enc_noise[(
            uint8_t)ref[pos]][(uint8_t)(*current_contig_it).read[read_offset]]);
        pos_var = static_cast<uint16_t>(read_offset - previous_noise_offset);
        metadata_output.noise_positions.push_back(pos_var);
        previous_noise_offset = read_offset;
      }
    }
    metadata_output.noise_serialized.push_back('\n');
    absolute_current_position = abs_pos + current_position;
    if (static_cast<uint64_t>(current_position) +
            (*current_contig_it).read_length >
        ref.size()) {
      throw std::runtime_error(
          "writecontig: read at pos=" + std::to_string(current_position) +
          " + len=" + std::to_string((*current_contig_it).read_length) +
          " exceeds ref.size()=" + std::to_string(ref.size()) +
          ", abs_pos=" + std::to_string(abs_pos));
    }
    metadata_output.position_entries.push_back(absolute_current_position);
    metadata_output.read_order_entries.push_back((*current_contig_it).order);
    metadata_output.read_length_entries.push_back(
        (*current_contig_it).read_length);
    metadata_output.orientation_entries.push_back((*current_contig_it).RC);
  }
  abs_pos += ref.size();
  return;
}

void getDataParams(encoder_global &eg, const compression_params &cp) {
  uint32_t clean_read_count;
  uint32_t total_read_count;
  clean_read_count =
      cp.read_info.num_reads_clean[0] + cp.read_info.num_reads_clean[1];
  total_read_count = cp.read_info.num_reads;

  std::ifstream singleton_count_input(eg.infile + ".singleton" + ".count",
                                      std::ifstream::in | std::ios::binary);
  singleton_count_input.read(byte_ptr(&eg.numreads_s), sizeof(uint32_t));
  singleton_count_input.close();
  const std::string singleton_count_path = eg.infile + ".singleton.count";
  safe_remove_file(singleton_count_path);
  eg.numreads = clean_read_count - eg.numreads_s;
  eg.numreads_N = total_read_count - clean_read_count;
  eg.bisulfite_ternary = cp.encoding.bisulfite_ternary;
}

void correct_order(uint32_t *order_s, const encoder_global &eg) {
  uint32_t total_read_count = eg.numreads + eg.numreads_s + eg.numreads_N;
  std::vector<uint8_t> is_n_read(total_read_count, 0);
  for (uint32_t i = 0; i < eg.numreads_N; i++) {
    is_n_read[order_s[eg.numreads_s + i]] = 1;
  }

  std::vector<uint32_t> cumulative_N_reads(eg.numreads + eg.numreads_s, 0);
  uint32_t clean_read_index = 0;
  uint32_t n_reads_seen = 0;
  for (uint32_t i = 0; i < total_read_count; i++) {
    if (is_n_read[i])
      n_reads_seen++;
    else
      cumulative_N_reads[clean_read_index++] = n_reads_seen;
  }

  // Insert the removed N-read slots back into singleton and clean-read
  // orderings.
  for (uint32_t i = 0; i < eg.numreads_s; i++)
    order_s[i] += cumulative_N_reads[order_s[i]];

  for (int thread_id = 0; thread_id < eg.num_thr; thread_id++) {
    rewrite_thread_order_file(thread_file_path(eg.infile_order, thread_id),
                              cumulative_N_reads);
  }
  safe_remove_file(eg.infile_order_N);
  return;
}

} // namespace spring
