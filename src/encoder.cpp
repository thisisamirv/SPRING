// Encodes reordered reads against their consensus/reference representation and
// writes the compressed sequence, position, noise, and unaligned side streams.

#include "encoder.h"
#include "libbsc/bsc.h"
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
                               const std::string &tail_path) {
  uint8_t base_to_int[128] = {};
  base_to_int[(uint8_t)'A'] = 0;
  base_to_int[(uint8_t)'C'] = 1;
  base_to_int[(uint8_t)'G'] = 2;
  base_to_int[(uint8_t)'T'] = 3;

  constexpr size_t input_buffer_size = 1 << 16;
  std::ifstream sequence_input(sequence_path, std::ios::binary);
  std::ofstream packed_output(packed_path, std::ios::binary);
  std::ofstream tail_output(tail_path, std::ios::binary);
  std::array<char, input_buffer_size> input_buffer{};
  std::array<char, input_buffer_size / 4 + 1> packed_buffer{};
  std::array<char, 4> trailing_bases{};
  uint64_t sequence_length = 0;
  size_t trailing_count = 0;

  while (sequence_input) {
    sequence_input.read(input_buffer.data(), input_buffer.size());
    const std::streamsize bytes_read = sequence_input.gcount();
    if (bytes_read <= 0) {
      break;
    }

    sequence_length += static_cast<uint64_t>(bytes_read);
    size_t packed_count = 0;

    for (std::streamsize input_index = 0; input_index < bytes_read;
         input_index++) {
      trailing_bases[trailing_count++] = input_buffer[input_index];
      if (trailing_count < trailing_bases.size()) {
        continue;
      }

      packed_buffer[packed_count++] =
          static_cast<char>(64 * base_to_int[(uint8_t)trailing_bases[3]] +
                            16 * base_to_int[(uint8_t)trailing_bases[2]] +
                            4 * base_to_int[(uint8_t)trailing_bases[1]] +
                            base_to_int[(uint8_t)trailing_bases[0]]);
      trailing_count = 0;
    }

    if (packed_count > 0) {
      packed_output.write(packed_buffer.data(),
                          static_cast<std::streamsize>(packed_count));
    }
  }

  if (trailing_count > 0) {
    uint8_t packed_byte = 0;
    for (size_t i = 0; i < trailing_count; ++i) {
      packed_byte |= (base_to_int[(uint8_t)trailing_bases[i]] << (2 * i));
    }
    packed_output.write(reinterpret_cast<const char *>(&packed_byte), 1);
  }

  return sequence_length;
}

void pack_sequence_chunk(const encoder_global &encoder_state,
                         const int thread_id,
                         uint64_t *thread_sequence_lengths) {
  const sequence_pack_paths paths =
      make_sequence_pack_paths(encoder_state.outfile_seq, thread_id);

  const uint64_t sequence_length = write_packed_sequence(
      paths.input_path, paths.packed_path, paths.tail_path);
  thread_sequence_lengths[thread_id] = sequence_length;

  remove(paths.input_path.c_str());
}

void calculate_sequence_lengths(const encoder_global &encoder_state,
                                uint64_t *thread_sequence_lengths) {
  for (int tid = 0; tid < encoder_state.num_thr; tid++) {
    std::string thread_seq_path =
        encoder_state.outfile_seq + '.' + std::to_string(tid);
    std::ifstream seq_in(thread_seq_path, std::ios::binary | std::ios::ate);
    if (!seq_in.is_open()) {
      thread_sequence_lengths[tid] = 0;
      continue;
    }
    thread_sequence_lengths[tid] = static_cast<uint64_t>(seq_in.tellg());
    seq_in.close();
  }
}

void pack_compress_seq(const encoder_global &encoder_state,
                       uint64_t *thread_sequence_lengths) {
  if (encoder_state.num_thr <= 0)
    return;
#pragma omp parallel num_threads(encoder_state.num_thr)
  {
    int tid = omp_get_thread_num();
    if (tid < encoder_state.num_thr) {
      pack_sequence_chunk(encoder_state, tid, thread_sequence_lengths);
    }
  }

  // Concatenate all thread-local packed files and compress as one monolithic
  // block.
  std::string monolithic_packed_path = encoder_state.outfile_seq + ".packed";
  std::ofstream monolithic_out(monolithic_packed_path, std::ios::binary);
  for (int tid = 0; tid < encoder_state.num_thr; tid++) {
    const sequence_pack_paths paths =
        make_sequence_pack_paths(encoder_state.outfile_seq, tid);
    std::ifstream chunk_in(paths.packed_path, std::ios::binary);
    if (chunk_in.is_open()) {
      monolithic_out << chunk_in.rdbuf();
      chunk_in.close();
      remove(paths.packed_path.c_str());
    }
  }
  monolithic_out.close();

  std::string monolithic_compressed_path = encoder_state.outfile_seq + ".bsc";
  bsc::BSC_compress(monolithic_packed_path.c_str(),
                    monolithic_compressed_path.c_str());
  remove(monolithic_packed_path.c_str());
}

void rewrite_thread_order_file(
    const std::string &order_path,
    const std::vector<uint32_t> &cumulative_n_reads) {
  const std::string order_tmp_path = order_path + ".tmp";
  std::ifstream order_input(order_path, std::ios::binary);
  std::ofstream order_output(order_tmp_path, std::ios::binary);
  uint32_t read_position;

  while (order_input.read(byte_ptr(&read_position), sizeof(uint32_t))) {
    read_position += cumulative_n_reads[read_position];
    order_output.write(byte_ptr(&read_position), sizeof(uint32_t));
  }

  remove(order_path.c_str());
  rename(order_tmp_path.c_str(), order_path.c_str());
}

std::string buildcontig(std::list<contig_reads> &current_contig,
                        const uint32_t &list_size) {
  static const char base_char_lookup[5] = {'A', 'C', 'G', 'T', 'N'};
  static const long base_index_lookup[128] = {
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 1, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 3, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
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
      uint8_t base_idx = base_index_lookup[(uint8_t)(*current_contig_it).read[i]];
      if (base_counts[idx][base_idx] < 65535) {
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
                 std::ofstream &f_pos, std::ofstream &f_noise,
                 std::ofstream &f_noisepos, std::ofstream &f_order,
                 std::ofstream &f_RC, std::ofstream &f_readlength,
                 const encoder_global &eg, uint64_t &abs_pos) {
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
        f_noise << eg.enc_noise[(
            uint8_t)ref[pos]][(uint8_t)(*current_contig_it).read[read_offset]];
        pos_var = static_cast<uint16_t>(read_offset - previous_noise_offset);
        f_noisepos.write(byte_ptr(&pos_var), sizeof(uint16_t));
        previous_noise_offset = read_offset;
      }
    }
    f_noise << "\n";
    absolute_current_position = abs_pos + current_position;
    f_pos.write(byte_ptr(&absolute_current_position), sizeof(uint64_t));
    f_order.write(byte_ptr(&((*current_contig_it).order)), sizeof(uint32_t));
    f_readlength.write(byte_ptr(&((*current_contig_it).read_length)),
                       sizeof(uint16_t));
    f_RC << (*current_contig_it).RC;
  }
  abs_pos += ref.size();
  return;
}

void getDataParams(encoder_global &eg, const compression_params &cp) {
  uint32_t clean_read_count;
  uint32_t total_read_count;
  clean_read_count = cp.num_reads_clean[0] + cp.num_reads_clean[1];
  total_read_count = cp.num_reads;

  std::ifstream singleton_count_input(eg.infile + ".singleton" + ".count",
                                      std::ifstream::in);
  singleton_count_input.read(byte_ptr(&eg.numreads_s), sizeof(uint32_t));
  singleton_count_input.close();
  const std::string singleton_count_path = eg.infile + ".singleton.count";
  remove(singleton_count_path.c_str());
  eg.numreads = clean_read_count - eg.numreads_s;
  eg.numreads_N = total_read_count - clean_read_count;

  Logger::log_info("Maximum Read length: " + std::to_string(eg.max_readlen));
  Logger::log_info("Number of non-singleton reads: " +
                   std::to_string(eg.numreads));
  Logger::log_info("Number of singleton reads: " + std::to_string(eg.numreads_s));
  Logger::log_info("Number of reads with N: " + std::to_string(eg.numreads_N));
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
  remove(eg.infile_order_N.c_str());
  return;
}

} // namespace spring
