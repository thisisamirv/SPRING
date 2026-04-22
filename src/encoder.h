#ifndef SPRING_ENCODER_H_
#define SPRING_ENCODER_H_

#include "params.h"
#include "raii.h"
#include <bitset>
#include <cstdint>
#include <fstream>
#include <list>
#include <omp.h>
#include <string>
#include <vector>

namespace spring {

// Forward declarations for types defined in bitset_dictionary.h or
// encoder_impl.h
struct bbhashdict;
struct compression_params;
template <size_t bitset_size> struct encoder_global_b;

struct encoder_global {
  uint32_t numreads, numreads_s, numreads_N;
  int numdict_s = NUM_DICT_ENCODER;

  int max_readlen, num_thr;

  std::string basedir;
  std::string infile;
  std::string infile_flag;
  std::string infile_pos;
  std::string infile_seq;
  std::string infile_RC;
  std::string infile_readlength;
  std::string infile_N;
  std::string outfile_unaligned;
  std::string outfile_seq;
  std::string outfile_pos;
  std::string outfile_noise;
  std::string outfile_noisepos;
  std::string infile_order;
  std::string infile_order_N;

  char enc_noise[128][128];
  bool methyl_ternary = false;
};

struct contig_reads {
  std::string read;
  int64_t pos;
  char RC;
  uint32_t order;
  uint16_t read_length;
};

// Non-template helpers (definitions in encoder.cpp)
std::string buildcontig(std::list<contig_reads> &current_contig,
                        const uint32_t &list_size);

void writecontig(const std::string &ref,
                 std::list<contig_reads> &current_contig, std::ofstream &f_seq,
                 std::ofstream &f_pos, std::ofstream &f_noise,
                 std::ofstream &f_noisepos, std::ofstream &f_order,
                 std::ofstream &f_RC, std::ofstream &f_readlength,
                 const encoder_global &eg, uint64_t &abs_pos);

void pack_compress_seq(const encoder_global &encoder_state,
                       uint64_t *thread_sequence_lengths);

void calculate_sequence_lengths(const encoder_global &encoder_state,
                                uint64_t *thread_sequence_lengths);

void getDataParams(encoder_global &eg, const compression_params &cp);

void correct_order(uint32_t *order_s, const encoder_global &eg);

// Template interface (definitions in encoder_impl.h)
template <size_t bitset_size>
std::string bitsettostring(std::bitset<bitset_size> encoded_bases,
                           const uint16_t readlen,
                           const encoder_global_b<bitset_size> &egb);

template <size_t bitset_size>
void encode(std::bitset<bitset_size> *reads, bbhashdict *dictionaries,
            uint32_t *read_orders, uint16_t *read_lengths,
            const std::vector<uint16_t> &all_read_lengths,
            bool *remaining_reads, OmpLock *read_locks,
            OmpLock *dictionary_locks, const encoder_global &eg,
            const encoder_global_b<bitset_size> &egb);

template <size_t bitset_size>
void setglobalarrays(encoder_global &eg, encoder_global_b<bitset_size> &egb);

template <size_t bitset_size>
void readsingletons(std::bitset<bitset_size> *read, uint32_t *order_s,
                    uint16_t *read_lengths_s, const encoder_global &eg,
                    const encoder_global_b<bitset_size> &egb);

template <size_t bitset_size>
void encoder_main(const std::string &temp_dir, compression_params &cp);

} // namespace spring

#endif // SPRING_ENCODER_H_
