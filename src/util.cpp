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

#include "util.h"
#include <algorithm>
#include <boost/filesystem.hpp>
#include <boost/iostreams/device/back_inserter.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_stream.hpp>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "id_compression/include/sam_block.h"
#include "omp.h"
#include "qvz/include/qvz.h"

namespace spring {

namespace {

const char *const kInvalidFastqError =
    "Invalid FASTQ(A) file. Number of lines not multiple of 4(2)";

struct read_range {
  uint64_t start;
  uint64_t end;
};

void write_fastq_record(std::ostream &out, const std::string &id,
                        const std::string &read,
                        const std::string *quality_or_null) {
  out << id << "\n";
  out << read << "\n";
  if (quality_or_null != nullptr) {
    out << "+\n";
    out << *quality_or_null << "\n";
  }
}

std::vector<read_range> compute_read_ranges(const uint32_t num_reads,
                                            const int num_thr) {
  if (num_thr <= 0)
    throw std::runtime_error("Number of threads must be positive.");

  std::vector<read_range> ranges(static_cast<size_t>(num_thr));
  const uint64_t num_reads_per_thread = 1 + ((num_reads - 1) / num_thr);
  uint64_t next_start = 0;
  for (int thread_index = 0; thread_index < num_thr; ++thread_index) {
    read_range &range = ranges[static_cast<size_t>(thread_index)];
    range.start = std::min<uint64_t>(next_start, num_reads);
    range.end = std::min<uint64_t>(range.start + num_reads_per_thread,
                                   num_reads);
    next_start = range.end;
  }
  return ranges;
}

bool matches_paired_id_code(const std::string &id_1, const std::string &id_2,
                            const uint8_t paired_id_code) {
  if (id_1.length() != id_2.length())
    return false;

  const size_t len = id_1.length();
  switch (paired_id_code) {
  case 1:
    if (id_1[len - 1] != '1' || id_2[len - 1] != '2')
      return false;
    for (size_t index = 0; index + 1 < len; ++index)
      if (id_1[index] != id_2[index])
        return false;
    return true;
  case 2:
    return id_1 == id_2;
  case 3: {
    size_t index = 0;
    while (index < len && id_1[index] == id_2[index]) {
      if (id_1[index] == ' ') {
        if (index + 1 < len && id_1[index + 1] == '1' &&
            id_2[index + 1] == '2') {
          ++index;
        } else {
          return false;
        }
      }
      ++index;
    }
    return index == len;
  }
  default:
    throw std::runtime_error("Invalid paired id code.");
  }
}

template <size_t BufferSize>
void write_encoded_read(const std::string &read, std::ofstream &fout,
                        const uint8_t (&dna_to_int)[128],
                        const uint8_t bits_per_base,
                        const uint8_t bases_per_byte) {
  uint8_t bitarray[BufferSize];
  uint8_t pos_in_bitarray = 0;
  const uint16_t readlen = read.size();
  fout.write(byte_ptr(&readlen), sizeof(uint16_t));

  const int full_groups = readlen / bases_per_byte;
  for (int group_index = 0; group_index < full_groups; ++group_index) {
    bitarray[pos_in_bitarray] = 0;
    for (int base_index = 0; base_index < bases_per_byte; ++base_index)
      bitarray[pos_in_bitarray] |= dna_to_int[static_cast<uint8_t>(
                                       read[bases_per_byte * group_index +
                                            base_index])]
                                   << (bits_per_base * base_index);
    ++pos_in_bitarray;
  }

  const int trailing_bases = readlen % bases_per_byte;
  if (trailing_bases != 0) {
    const int group_index = full_groups;
    bitarray[pos_in_bitarray] = 0;
    for (int base_index = 0; base_index < trailing_bases; ++base_index)
      bitarray[pos_in_bitarray] |= dna_to_int[static_cast<uint8_t>(
                                       read[bases_per_byte * group_index +
                                            base_index])]
                                   << (bits_per_base * base_index);
    ++pos_in_bitarray;
  }

  fout.write(byte_ptr(&bitarray[0]), pos_in_bitarray);
}

template <size_t BufferSize, size_t AlphabetSize>
void read_encoded_read(std::string &read, std::ifstream &fin,
                       const char (&int_to_dna)[AlphabetSize],
                       const uint8_t bit_mask,
                       const uint8_t bits_per_base,
                       const uint8_t bases_per_byte) {
  uint16_t readlen;
  uint8_t bitarray[BufferSize];
  fin.read(byte_ptr(&readlen), sizeof(uint16_t));
  read.resize(readlen);

  const uint16_t num_bytes_to_read =
      ((uint32_t)readlen + bases_per_byte - 1) / bases_per_byte;
  fin.read(byte_ptr(&bitarray[0]), num_bytes_to_read);

  uint8_t pos_in_bitarray = 0;
  const int full_groups = readlen / bases_per_byte;
  for (int group_index = 0; group_index < full_groups; ++group_index) {
    for (int base_index = 0; base_index < bases_per_byte; ++base_index) {
      read[bases_per_byte * group_index + base_index] =
          int_to_dna[bitarray[pos_in_bitarray] & bit_mask];
      bitarray[pos_in_bitarray] >>= bits_per_base;
    }
    ++pos_in_bitarray;
  }

  const int trailing_bases = readlen % bases_per_byte;
  if (trailing_bases != 0) {
    const int group_index = full_groups;
    for (int base_index = 0; base_index < trailing_bases; ++base_index) {
      read[bases_per_byte * group_index + base_index] =
          int_to_dna[bitarray[pos_in_bitarray] & bit_mask];
      bitarray[pos_in_bitarray] >>= bits_per_base;
    }
  }
}

void fill_reverse_complement(const char *input_bases, char *output_bases,
                             const int readlen) {
  for (int index = 0; index < readlen; ++index)
    output_bases[index] = chartorevchar[static_cast<uint8_t>(
        input_bases[readlen - index - 1])];
}

} // namespace

uint32_t read_fastq_block(std::istream *input_stream, std::string *id_array,
                          std::string *read_array, std::string *quality_array,
                          const uint32_t &num_reads, const bool &fasta_flag) {
  uint32_t reads_processed = 0;
  std::string comment_line;
  for (; reads_processed < num_reads; reads_processed++) {
    if (!std::getline(*input_stream, id_array[reads_processed]))
      break;
    remove_CR_from_end(id_array[reads_processed]);
    if (!std::getline(*input_stream, read_array[reads_processed]))
      throw std::runtime_error(kInvalidFastqError);
    remove_CR_from_end(read_array[reads_processed]);
    if (fasta_flag)
      continue;
    if (!std::getline(*input_stream, comment_line))
      throw std::runtime_error(kInvalidFastqError);
    if (!std::getline(*input_stream, quality_array[reads_processed]))
      throw std::runtime_error(kInvalidFastqError);
    remove_CR_from_end(quality_array[reads_processed]);
  }
  return reads_processed;
}

void write_fastq_block(std::ofstream &output_stream, std::string *id_array,
                       std::string *read_array, std::string *quality_array,
                       const uint32_t &num_reads, const bool preserve_quality,
                       const int &num_thr, const bool &gzip_flag,
                       const int &gzip_level) {
  const std::string *quality_or_null = preserve_quality ? quality_array : nullptr;
  if (!gzip_flag) {
    for (uint32_t read_index = 0; read_index < num_reads; read_index++)
      write_fastq_record(output_stream, id_array[read_index],
                         read_array[read_index],
                         quality_or_null == nullptr ? nullptr
                                                    : &quality_or_null[read_index]);
  } else {
    if (num_reads == 0)
      return;

    std::vector<std::string> gzip_compressed(static_cast<size_t>(num_thr));
    const std::vector<read_range> thread_ranges =
        compute_read_ranges(num_reads, num_thr);
#pragma omp parallel num_threads(num_thr)
    {
      const int tid = omp_get_thread_num();
      boost::iostreams::filtering_ostream out;
      out.push(boost::iostreams::gzip_compressor(
          boost::iostreams::gzip_params(gzip_level)));
      out.push(boost::iostreams::back_inserter(gzip_compressed[tid]));

      const read_range &range = thread_ranges[static_cast<size_t>(tid)];
      for (uint64_t i = range.start; i < range.end; i++)
        write_fastq_record(out, id_array[i], read_array[i],
                           quality_or_null == nullptr ? nullptr
                                                      : &quality_or_null[i]);
      boost::iostreams::close(out);

    } // end omp parallel
    for (uint32_t thread_index = 0; thread_index < (uint32_t)num_thr;
         thread_index++)
      output_stream.write(&(gzip_compressed[thread_index][0]),
                          gzip_compressed[thread_index].size());
  }
}

void compress_id_block(const char *output_path, std::string *id_array,
                       const uint32_t &num_ids) {
  struct id_comp::compressor_info_t comp_info;
  comp_info.numreads = num_ids;
  comp_info.mode = COMPRESSION;
  comp_info.id_array = id_array;
  comp_info.fcomp = fopen(output_path, "w");
  if (!comp_info.fcomp) {
    perror(output_path);
    throw std::runtime_error("ID compression: File output error");
  }
  id_comp::compress((void *)&comp_info);
  fclose(comp_info.fcomp);
}

void decompress_id_block(const char *input_path, std::string *id_array,
                         const uint32_t &num_ids) {
  struct id_comp::compressor_info_t comp_info;
  comp_info.numreads = num_ids;
  comp_info.mode = DECOMPRESSION;
  comp_info.id_array = id_array;
  comp_info.fcomp = fopen(input_path, "r");
  if (!comp_info.fcomp) {
    perror(input_path);
    throw std::runtime_error("ID compression: File input error");
  }
  id_comp::decompress((void *)&comp_info);
  fclose(comp_info.fcomp);
}

void quantize_quality(std::string *quality_array, const uint32_t &num_lines,
                      char *quantization_table) {
  for (uint32_t i = 0; i < num_lines; i++)
    for (uint32_t j = 0; j < quality_array[i].size(); j++)
      quality_array[i][j] = quantization_table[(uint8_t)quality_array[i][j]];
  return;
}

void quantize_quality_qvz(std::string *quality_array, const uint32_t &num_lines,
                          uint32_t *str_len_array, double qv_ratio) {
  struct qvz::qv_options_t opts;
  opts.verbose = 0;
  opts.stats = 0;
  opts.clusters = 1;
  opts.uncompressed = 0;
  opts.ratio = qv_ratio;
  opts.distortion = DISTORTION_MSE;
  opts.mode = MODE_FIXED;
  size_t max_readlen =
      *(std::max_element(str_len_array, str_len_array + num_lines));
  qvz::encode(&opts, max_readlen, num_lines, quality_array, str_len_array);
}

void generate_illumina_binning_table(char *illumina_binning_table) {
  for (uint8_t i = 0; i <= 33 + 1; i++)
    illumina_binning_table[i] = 33 + 0;
  for (uint8_t i = 33 + 2; i <= 33 + 9; i++)
    illumina_binning_table[i] = 33 + 6;
  for (uint8_t i = 33 + 10; i <= 33 + 19; i++)
    illumina_binning_table[i] = 33 + 15;
  for (uint8_t i = 33 + 20; i <= 33 + 24; i++)
    illumina_binning_table[i] = 33 + 22;
  for (uint8_t i = 33 + 25; i <= 33 + 29; i++)
    illumina_binning_table[i] = 33 + 27;
  for (uint8_t i = 33 + 30; i <= 33 + 34; i++)
    illumina_binning_table[i] = 33 + 33;
  for (uint8_t i = 33 + 35; i <= 33 + 39; i++)
    illumina_binning_table[i] = 33 + 37;
  for (uint8_t i = 33 + 40; i <= 127; i++)
    illumina_binning_table[i] = 33 + 40;
}

void generate_binary_binning_table(char *binary_binning_table,
                                   const unsigned int thr,
                                   const unsigned int high,
                                   const unsigned int low) {
  const unsigned int split = std::min(127U, 33U + thr);
  for (unsigned int i = 0; i < split; i++)
    binary_binning_table[i] = 33 + low;
  for (unsigned int i = split; i <= 127; i++)
    binary_binning_table[i] = 33 + high;
}

// Detect the paired-id conventions Spring knows how to reconstruct implicitly.
uint8_t find_id_pattern(const std::string &id_1, const std::string &id_2) {
  for (uint8_t paired_id_code = 1; paired_id_code <= 3; ++paired_id_code)
    if (matches_paired_id_code(id_1, id_2, paired_id_code))
      return paired_id_code;
  return 0;
}

bool check_id_pattern(const std::string &id_1, const std::string &id_2,
                      const uint8_t paired_id_code) {
  return matches_paired_id_code(id_1, id_2, paired_id_code);
}

void modify_id(std::string &id, const uint8_t paired_id_code) {
  if (paired_id_code == 2)
    return;
  else if (paired_id_code == 1) {
    id.back() = '2';
    return;
  } else if (paired_id_code == 3) {
    int i = 0;
    while (id[i] != ' ')
      i++;
    id[i + 1] = '2';
    return;
  }
}

void write_dna_in_bits(const std::string &read, std::ofstream &fout) {
  uint8_t dna2int[128] = {};
  dna2int[(uint8_t)'A'] = 0;
  dna2int[(uint8_t)'C'] = 2;
  dna2int[(uint8_t)'G'] = 1;
  dna2int[(uint8_t)'T'] = 3;
  write_encoded_read<128>(read, fout, dna2int, 2, 4);
}

void read_dna_from_bits(std::string &read, std::ifstream &fin) {
  const char int2dna[4] = {'A', 'G', 'C', 'T'};
  read_encoded_read<128>(read, fin, int2dna, 3, 2, 4);
}

void write_dnaN_in_bits(const std::string &read, std::ofstream &fout) {
  uint8_t dna2int[128] = {};
  dna2int[(uint8_t)'A'] = 0;
  dna2int[(uint8_t)'C'] = 2;
  dna2int[(uint8_t)'G'] = 1;
  dna2int[(uint8_t)'T'] = 3;
  dna2int[(uint8_t)'N'] = 4;
  write_encoded_read<256>(read, fout, dna2int, 4, 2);
}

void read_dnaN_from_bits(std::string &read, std::ifstream &fin) {
  const char int2dna[5] = {'A', 'G', 'C', 'T', 'N'};
  read_encoded_read<256>(read, fin, int2dna, 15, 4, 2);
}

void reverse_complement(char *input_bases, char *output_bases,
                        const int readlen) {
  fill_reverse_complement(input_bases, output_bases, readlen);
  output_bases[readlen] = '\0';
}

std::string reverse_complement(const std::string &input_bases,
                               const int readlen) {
  std::string output_bases(readlen, '\0');
  fill_reverse_complement(input_bases.data(), &output_bases[0], readlen);
  return output_bases;
}

void remove_CR_from_end(std::string &str) {
  if (!str.empty() && str.back() == '\r')
    str.resize(str.size() - 1);
}

size_t get_directory_size(const std::string &temp_dir) {
  namespace fs = boost::filesystem;
  size_t size = 0;
  fs::path p{temp_dir};
  fs::directory_iterator itr{p};
  for (; itr != fs::directory_iterator{}; ++itr) {
    size += fs::file_size(itr->path());
  }
  return size;
}

// Use zigzag plus varint encoding for compact temporary integer streams.
uint64_t zigzag_encode64(const int64_t value) {
  const uint64_t sign_mask = (value < 0) ? UINT64_MAX : 0U;
  return (static_cast<uint64_t>(value) << 1) ^ sign_mask;
}

int64_t zigzag_decode64(const uint64_t value) {
  return static_cast<int64_t>(value >> 1) ^ -static_cast<int64_t>(value & 1U);
}

void write_var_int64(const int64_t value, std::ofstream &output_stream) {
  uint64_t encoded_value = zigzag_encode64(value);
  uint8_t byte;
  while (encoded_value > 127) {
    byte = (uint8_t)(encoded_value & 0x7f) | 0x80;
    output_stream.write(byte_ptr(&byte), sizeof(uint8_t));
    encoded_value >>= 7;
  }
  byte = (uint8_t)(encoded_value & 0x7f);
  output_stream.write(byte_ptr(&byte), sizeof(uint8_t));
}

int64_t read_var_int64(std::ifstream &input_stream) {
  uint64_t encoded_value = 0;
  uint8_t byte;
  uint8_t shift = 0;
  do {
    input_stream.read(byte_ptr(&byte), sizeof(uint8_t));
    encoded_value |= ((byte & 0x7f) << shift);
    shift += 7;
  } while (byte & 0x80);
  return zigzag_decode64(encoded_value);
}

} // namespace spring
