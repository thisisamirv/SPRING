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

#ifndef SPRING_ENCODER_H_
#define SPRING_ENCODER_H_

#include "bitset_util.h"
#include "params.h"
#include "util.h"
#include <array>
#include <algorithm>
#include <bitset>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <list>
#include <omp.h>
#include <string>
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
};

struct contig_reads {
  std::string read;
  int64_t pos;
  char RC;
  uint32_t order;
  uint16_t read_length;
};

std::string buildcontig(std::list<contig_reads> &current_contig,
                        const uint32_t &list_size);

void writecontig(const std::string &ref,
                 std::list<contig_reads> &current_contig, std::ofstream &f_seq,
                 std::ofstream &f_pos, std::ofstream &f_noise,
                 std::ofstream &f_noisepos, std::ofstream &f_order,
                 std::ofstream &f_RC, std::ofstream &f_readlength,
                 const encoder_global &eg, uint64_t &abs_pos);

void pack_compress_seq(const encoder_global &eg, uint64_t *file_len_seq_thr);

void getDataParams(encoder_global &eg, const compression_params &cp);

void correct_order(uint32_t *order_s, const encoder_global &eg);

inline void initialize_encoder_dict_ranges(
    std::array<bbhashdict, NUM_DICT_ENCODER> &dict, const int max_readlen) {
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
std::string bitsettostring(std::bitset<bitset_size> b, const uint16_t readlen,
                           const encoder_global_b<bitset_size> &egb) {
  static const char revinttochar[8] = {'A', 'N', 'G', 0, 'C', 0, 'T', 0};
  std::string s;
  s.resize(readlen);
  unsigned long long ull;
  for (int i = 0; i < 3 * readlen / 63 + 1; i++) {
    ull = (b & egb.mask63).to_ullong();
    b >>= 63;
    for (int j = 21 * i; j < 21 * i + 21 && j < readlen; j++) {
      s[j] = revinttochar[ull % 8];
      ull /= 8;
    }
  }
  return s;
}

template <size_t bitset_size>
void encode(std::bitset<bitset_size> *read, bbhashdict *dict, uint32_t *order_s,
            uint16_t *read_lengths_s, const encoder_global &eg,
            const encoder_global_b<bitset_size> &egb) {
  static const int thresh_s = THRESH_ENCODER;
  static const int maxsearch = MAX_SEARCH_ENCODER;
  omp_lock_t *read_lock = new omp_lock_t[eg.numreads_s + eg.numreads_N];
  omp_lock_t *dict_lock = new omp_lock_t[eg.numreads_s + eg.numreads_N];
  for (uint64_t j = 0; j < eg.numreads_s + eg.numreads_N; j++) {
    omp_init_lock(&read_lock[j]);
    omp_init_lock(&dict_lock[j]);
  }
  bool *remainingreads = new bool[eg.numreads_s + eg.numreads_N];
  std::fill(remainingreads, remainingreads + eg.numreads_s + eg.numreads_N, 1);

  std::bitset<bitset_size> *mask1 = new std::bitset<bitset_size>[eg.numdict_s];
  generateindexmasks<bitset_size>(mask1, dict, eg.numdict_s, 3);
  std::bitset<bitset_size> **mask =
      new std::bitset<bitset_size> *[eg.max_readlen];
  for (int i = 0; i < eg.max_readlen; i++)
    mask[i] = new std::bitset<bitset_size>[eg.max_readlen];
  generatemasks<bitset_size>(mask, eg.max_readlen, 3);
  std::cout << "Encoding reads\n";
#pragma omp parallel
  {
    int tid = omp_get_thread_num();
    std::ifstream f(eg.infile + '.' + std::to_string(tid), std::ios::binary);

    std::ifstream fin_flag(eg.infile_flag + '.' + std::to_string(tid));
    boost::iostreams::filtering_streambuf<boost::iostreams::input> inbuf_flag;
    inbuf_flag.push(boost::iostreams::gzip_decompressor());
    inbuf_flag.push(fin_flag);
    std::istream in_flag(&inbuf_flag);
    std::ifstream fin_pos(eg.infile_pos + '.' + std::to_string(tid),
                          std::ios::binary);
    boost::iostreams::filtering_streambuf<boost::iostreams::input> inbuf_pos;
    inbuf_pos.push(boost::iostreams::gzip_decompressor());
    inbuf_pos.push(fin_pos);
    std::istream in_pos(&inbuf_pos);
    std::ifstream in_order(eg.infile_order + '.' + std::to_string(tid),
                           std::ios::binary);
    std::ifstream fin_RC(eg.infile_RC + '.' + std::to_string(tid));
    boost::iostreams::filtering_streambuf<boost::iostreams::input> inbuf_RC;
    inbuf_RC.push(boost::iostreams::gzip_decompressor());
    inbuf_RC.push(fin_RC);
    std::istream in_RC(&inbuf_RC);
    std::ifstream fin_readlength(
        eg.infile_readlength + '.' + std::to_string(tid), std::ios::binary);
    boost::iostreams::filtering_streambuf<boost::iostreams::input>
        inbuf_readlength;
    inbuf_readlength.push(boost::iostreams::gzip_decompressor());
    inbuf_readlength.push(fin_readlength);
    std::istream in_readlength(&inbuf_readlength);
    std::ofstream f_seq(eg.outfile_seq + '.' + std::to_string(tid));
    std::ofstream f_pos(eg.outfile_pos + '.' + std::to_string(tid),
                        std::ios::binary);
    std::ofstream f_noise(eg.outfile_noise + '.' + std::to_string(tid));
    std::ofstream f_noisepos(eg.outfile_noisepos + '.' + std::to_string(tid),
                             std::ios::binary);
    std::ofstream f_order(eg.infile_order + '.' + std::to_string(tid) + ".tmp",
                          std::ios::binary);
    std::ofstream f_RC(eg.infile_RC + '.' + std::to_string(tid) + ".tmp");
    std::ofstream f_readlength(eg.infile_readlength + '.' +
                                   std::to_string(tid) + ".tmp",
                               std::ios::binary);

    int64_t dictidx[2];
    uint64_t startposidx;
    uint64_t ull;
    uint64_t abs_pos = 0;
    bool flag = 0;
    std::string current, ref;
    std::bitset<bitset_size> forward_bitset, reverse_bitset, b;
    char c = '0', rc = 'd';
    std::list<contig_reads> current_contig;
    int64_t p;
    uint16_t rl;
    uint32_t ord, list_size = 0;
    std::array<std::list<uint32_t>, NUM_DICT_ENCODER> deleted_rids;
    bool done = false;
    while (!done) {
      if (!(in_flag >> c))
        done = true;
      if (!done) {
        read_dna_from_bits(current, f);
        rc = in_RC.get();
        in_pos.read(byte_ptr(&p), sizeof(int64_t));
        in_order.read(byte_ptr(&ord), sizeof(uint32_t));
        in_readlength.read(byte_ptr(&rl), sizeof(uint16_t));
      }
      if (c == '0' || done || list_size > 10000000)
      {
        if (list_size != 0) {
          current_contig.sort([](const contig_reads &a, const contig_reads &b) {
            return a.pos < b.pos;
          });
          auto current_contig_it = current_contig.begin();
          int64_t first_pos = (*current_contig_it).pos;
          for (; current_contig_it != current_contig.end(); ++current_contig_it)
            (*current_contig_it).pos -= first_pos;

          ref = buildcontig(current_contig, list_size);
          if ((int64_t)ref.size() >= eg.max_readlen &&
              (eg.numreads_s + eg.numreads_N > 0)) {
            // Scan each contig window for singleton reads that can be folded in.
            forward_bitset.reset();
            reverse_bitset.reset();
            stringtobitset(ref.substr(0, eg.max_readlen), eg.max_readlen,
                           forward_bitset, egb.basemask);
            stringtobitset(reverse_complement(ref.substr(0, eg.max_readlen),
                                              eg.max_readlen),
                           eg.max_readlen, reverse_bitset, egb.basemask);
            for (long j = 0; j < (int64_t)ref.size() - eg.max_readlen + 1;
                 j++) {
              for (int rev = 0; rev < 2; rev++) {
                for (int l = 0; l < eg.numdict_s; l++) {
                  if (!rev)
                    b = forward_bitset & mask1[l];
                  else
                    b = reverse_bitset & mask1[l];
                  ull = (b >> 3 * dict[l].start).to_ullong();
                  startposidx = dict[l].bphf->lookup(ull);
                  if (startposidx >= dict[l].numkeys)
                    continue;
                  if (!omp_test_lock(&dict_lock[startposidx]))
                    continue;
                  dict[l].findpos(dictidx, startposidx);
                  if (dict[l].empty_bin[startposidx]) {
                    omp_unset_lock(&dict_lock[startposidx]);
                    continue;
                  }
                  uint64_t ull1 =
                      ((read[dict[l].read_id[dictidx[0]]] & mask1[l]) >>
                       3 * dict[l].start)
                          .to_ullong();
                    if (ull == ull1) {
                    for (int64_t i = dictidx[1] - 1;
                         i >= dictidx[0] && i >= dictidx[1] - maxsearch; i--) {
                      auto rid = dict[l].read_id[i];
                      int hamming;
                      if (!rev)
                        hamming =
                            ((forward_bitset ^ read[rid]) &
                             mask[0][eg.max_readlen - read_lengths_s[rid]])
                                .count();
                      else
                        hamming =
                            ((reverse_bitset ^ read[rid]) &
                             mask[0][eg.max_readlen - read_lengths_s[rid]])
                                .count();
                      if (hamming <= thresh_s) {
                        if (!omp_test_lock(&read_lock[rid]))
                          continue;
                        if (remainingreads[rid]) {
                          remainingreads[rid] = 0;
                          flag = 1;
                        }
                        omp_unset_lock(&read_lock[rid]);
                      }
                      if (flag == 1) {
                        flag = 0;
                        list_size++;
                        char rc = rev ? 'r' : 'd';
                        long pos =
                            rev ? (j + eg.max_readlen - read_lengths_s[rid])
                                : j;
                        std::string read_string =
                            rev ? reverse_complement(
                                      bitsettostring<bitset_size>(
                                          read[rid], read_lengths_s[rid], egb),
                                      read_lengths_s[rid])
                                : bitsettostring<bitset_size>(
                                      read[rid], read_lengths_s[rid], egb);
                        current_contig.push_back({read_string, pos, rc,
                                                  order_s[rid],
                                                  read_lengths_s[rid]});
                        for (int l1 = 0; l1 < eg.numdict_s; l1++) {
                          if (read_lengths_s[rid] > dict[l1].end)
                            deleted_rids[l1].push_back(rid);
                        }
                      }
                    }
                  }
                  omp_unset_lock(&dict_lock[startposidx]);
                  for (int l1 = 0; l1 < eg.numdict_s; l1++)
                    for (auto it = deleted_rids[l1].begin();
                         it != deleted_rids[l1].end();) {
                      b = read[*it] & mask1[l1];
                      ull = (b >> 3 * dict[l1].start).to_ullong();
                      startposidx = dict[l1].bphf->lookup(ull);
                      if (!omp_test_lock(&dict_lock[startposidx])) {
                        ++it;
                        continue;
                      }
                      dict[l1].findpos(dictidx, startposidx);
                      // Remove matched singletons once they have been absorbed.
                      dict[l1].remove(dictidx, startposidx, *it);
                      it = deleted_rids[l1].erase(it);
                      omp_unset_lock(&dict_lock[startposidx]);
                    }
                }
              }
              if (j != (int64_t)ref.size() - eg.max_readlen) {
                forward_bitset >>= 3;
                forward_bitset = forward_bitset & mask[0][0];
                forward_bitset |=
                    egb.basemask[eg.max_readlen - 1]
                                [(uint8_t)ref[j + eg.max_readlen]];
                reverse_bitset <<= 3;
                reverse_bitset = reverse_bitset & mask[0][0];
                reverse_bitset |= egb.basemask[0][(
                    uint8_t)chartorevchar[(uint8_t)ref[j + eg.max_readlen]]];
              }

            }
          }
          current_contig.sort([](const contig_reads &a, const contig_reads &b) {
            return a.pos < b.pos;
          });
          writecontig(ref, current_contig, f_seq, f_pos, f_noise, f_noisepos,
                      f_order, f_RC, f_readlength, eg, abs_pos);
        }
        if (!done) {
          current_contig = {{current, p, rc, ord, rl}};
          list_size = 1;
        }
      } else if (c == '1') // read found during rightward search
      {
        current_contig.push_back({current, p, rc, ord, rl});
        list_size++;
      }
    }
    f.close();
    fin_flag.close();
    fin_pos.close();
    in_order.close();
    fin_RC.close();
    fin_readlength.close();
    f_seq.close();
    f_pos.close();
    f_noise.close();
    f_noisepos.close();
    f_order.close();
    f_readlength.close();
    f_RC.close();
  }

  // Stitch the per-thread streams back into the final encoded outputs.
  std::ofstream f_order(eg.infile_order, std::ios::binary);
  std::ofstream f_readlength(eg.infile_readlength, std::ios::binary);
  std::ofstream f_noisepos(eg.outfile_noisepos, std::ios::binary);
  std::ofstream f_noise(eg.outfile_noise);
  std::ofstream f_RC(eg.infile_RC);

  for (int tid = 0; tid < eg.num_thr; tid++) {
    std::ifstream in_order(eg.infile_order + '.' + std::to_string(tid) + ".tmp",
                           std::ios::binary);
    std::ifstream in_readlength(eg.infile_readlength + '.' +
                                    std::to_string(tid) + ".tmp",
                                std::ios::binary);
    std::ifstream in_RC(eg.infile_RC + '.' + std::to_string(tid) + ".tmp");
    std::ifstream in_noisepos(eg.outfile_noisepos + '.' + std::to_string(tid),
                              std::ios::binary);
    std::ifstream in_noise(eg.outfile_noise + '.' + std::to_string(tid));
    f_order << in_order.rdbuf();
    f_order.clear(); // clear error flag in case in_order is empty
    f_noisepos << in_noisepos.rdbuf();
    f_noisepos.clear(); // clear error flag in case in_noisepos is empty
    f_noise << in_noise.rdbuf();
    f_noise.clear(); // clear error flag in case in_noise is empty
    f_readlength << in_readlength.rdbuf();
    f_readlength.clear(); // clear error flag in case in_readlength is empty
    f_RC << in_RC.rdbuf();
    f_RC.clear(); // clear error flag in case in_RC is empty

    remove((eg.infile_order + '.' + std::to_string(tid)).c_str());
    remove((eg.infile_order + '.' + std::to_string(tid) + ".tmp").c_str());
    remove((eg.infile_readlength + '.' + std::to_string(tid)).c_str());
    remove((eg.infile_readlength + '.' + std::to_string(tid) + ".tmp").c_str());
    remove((eg.outfile_noisepos + '.' + std::to_string(tid)).c_str());
    remove((eg.outfile_noise + '.' + std::to_string(tid)).c_str());
    remove((eg.infile_RC + '.' + std::to_string(tid) + ".tmp").c_str());
    remove((eg.infile_RC + '.' + std::to_string(tid)).c_str());
    remove((eg.infile_flag + '.' + std::to_string(tid)).c_str());
    remove((eg.infile_pos + '.' + std::to_string(tid)).c_str());
    remove((eg.infile + '.' + std::to_string(tid)).c_str());
  }
  f_order.close();
  f_readlength.close();
  std::ofstream f_unaligned(eg.outfile_unaligned, std::ios::binary);
  f_order.open(eg.infile_order, std::ios::binary | std::ofstream::app);
  f_readlength.open(eg.infile_readlength,
                    std::ios::binary | std::ofstream::app);
  uint32_t matched_s = eg.numreads_s;
  uint64_t len_unaligned = 0;
  for (uint32_t i = 0; i < eg.numreads_s; i++)
    if (remainingreads[i] == 1) {
      matched_s--;
      f_order.write(byte_ptr(&order_s[i]), sizeof(uint32_t));
      f_readlength.write(byte_ptr(&read_lengths_s[i]), sizeof(uint16_t));
      std::string unaligned_read =
          bitsettostring<bitset_size>(read[i], read_lengths_s[i], egb);
      write_dnaN_in_bits(unaligned_read, f_unaligned);
      len_unaligned += read_lengths_s[i];
    }
  uint32_t matched_N = eg.numreads_N;
  for (uint32_t i = eg.numreads_s; i < eg.numreads_s + eg.numreads_N; i++)
    if (remainingreads[i] == 1) {
      matched_N--;
      std::string unaligned_read =
          bitsettostring<bitset_size>(read[i], read_lengths_s[i], egb);
      write_dnaN_in_bits(unaligned_read, f_unaligned);
      f_order.write(byte_ptr(&order_s[i]), sizeof(uint32_t));
      f_readlength.write(byte_ptr(&read_lengths_s[i]), sizeof(uint16_t));
      len_unaligned += read_lengths_s[i];
    }
  f_order.close();
  f_readlength.close();
  f_unaligned.close();
  delete[] remainingreads;
  delete[] dict_lock;
  delete[] read_lock;
  for (int i = 0; i < eg.max_readlen; i++)
    delete[] mask[i];
  delete[] mask;
  delete[] mask1;

  std::ofstream f_unaligned_count(eg.outfile_unaligned + ".count",
                                  std::ios::binary);
  f_unaligned_count.write(byte_ptr(&len_unaligned), sizeof(uint64_t));
  f_unaligned_count.close();

  // Pack contig sequence payloads before rewriting positions as absolute offsets.
  std::vector<uint64_t> file_len_seq_thr(static_cast<size_t>(eg.num_thr));
  uint64_t abs_pos = 0;
  uint64_t abs_pos_thr;
  pack_compress_seq(eg, file_len_seq_thr.data());
  std::ofstream fout_pos(eg.outfile_pos, std::ios::binary);
  for (int tid = 0; tid < eg.num_thr; tid++) {
    std::ifstream fin_pos(eg.outfile_pos + '.' + std::to_string(tid),
                          std::ios::binary);
    fin_pos.read(byte_ptr(&abs_pos_thr), sizeof(uint64_t));
    while (!fin_pos.eof()) {
      abs_pos_thr += abs_pos;
      fout_pos.write(byte_ptr(&abs_pos_thr), sizeof(uint64_t));
      fin_pos.read(byte_ptr(&abs_pos_thr), sizeof(uint64_t));
    }
    fin_pos.close();
    remove((eg.outfile_pos + '.' + std::to_string(tid)).c_str());
    abs_pos += file_len_seq_thr[tid];
  }
  fout_pos.close();

  std::cout << "Encoding done:\n";
  std::cout << matched_N << " reads with N were aligned\n";
  return;
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
void encoder_main(const std::string &temp_dir, const compression_params &cp) {
  encoder_global_b<bitset_size> egb(cp.max_readlen);
  encoder_global eg;

  eg.basedir = temp_dir;
  eg.infile = eg.basedir + "/temp.dna";
  eg.infile_pos = eg.basedir + "/temppos.txt";
  eg.infile_flag = eg.basedir + "/tempflag.txt";
  eg.infile_order = eg.basedir + "/read_order.bin";
  eg.infile_order_N = eg.basedir + "/read_order_N.bin";
  eg.infile_RC = eg.basedir + "/read_rev.txt";
  eg.infile_readlength = eg.basedir + "/read_lengths.bin";
  eg.infile_N = eg.basedir + "/input_N.dna";
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
  readsingletons<bitset_size>(read, order_s, read_lengths_s, eg, egb);
  remove(eg.infile_N.c_str());
  correct_order(order_s, eg);

  std::array<bbhashdict, NUM_DICT_ENCODER> dict;
  initialize_encoder_dict_ranges(dict, eg.max_readlen);
  if (singleton_pool_size > 0)
    constructdictionary<bitset_size>(read, dict.data(), read_lengths_s,
                                     eg.numdict_s, singleton_pool_size, 3,
                                     eg.basedir, eg.num_thr);
  encode<bitset_size>(read, dict.data(), order_s, read_lengths_s, eg, egb);

  delete[] read;
  delete[] order_s;
  delete[] read_lengths_s;
}

} // namespace spring

#endif // SPRING_ENCODER_H_
