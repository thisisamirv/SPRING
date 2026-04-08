// Declares shared utility types and helpers for FASTQ blocks, ids, quality
// processing, DNA packing, reverse complements, and temporary stream I/O.

#ifndef SPRING_UTIL_H_
#define SPRING_UTIL_H_

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <istream>
#include <streambuf>
#include <string>

#include <zlib.h>

namespace spring {

template <typename T> inline char *byte_ptr(T *value) {
  return reinterpret_cast<char *>(value);
}

template <typename T> inline const char *byte_ptr(const T *value) {
  return reinterpret_cast<const char *>(value);
}

static const char chartorevchar[128] = {
    0, 0,   0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0,   0, 0, 0,
    0, 0,   0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0,   0, 0, 0,
    0, 0,   0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0,   0, 0, 'T',
    0, 'G', 0, 0, 0, 'C', 0, 0, 0, 0, 0, 0, 'N', 0, 0, 0, 0, 0, 'A', 0, 0, 0,
    0, 0,   0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0,   0, 0, 0,
    0, 0,   0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0};

struct compression_params {
  bool paired_end;
  bool preserve_order;
  bool preserve_quality;
  bool preserve_id;
  bool long_flag;
  bool qvz_flag;
  bool ill_bin_flag;
  bool bin_thr_flag;
  double qvz_ratio;
  unsigned int bin_thr_thr;
  unsigned int bin_thr_high;
  unsigned int bin_thr_low;
  uint32_t num_reads;
  uint32_t num_reads_clean[2];
  uint32_t max_readlen;
  uint8_t paired_id_code;
  bool paired_id_match;
  int num_reads_per_block;
  int num_reads_per_block_long;
  int num_thr;
  int compression_level;
  static constexpr size_t kFileLenThrSize = 1024;
  uint64_t file_len_seq_thr[kFileLenThrSize];
  uint64_t file_len_id_thr[kFileLenThrSize];
  bool use_crlf;
};

class gzip_istreambuf : public std::streambuf {
public:
  gzip_istreambuf();
  explicit gzip_istreambuf(const std::string &path);
  gzip_istreambuf(const gzip_istreambuf &) = delete;
  gzip_istreambuf &operator=(const gzip_istreambuf &) = delete;
  ~gzip_istreambuf() override;

  bool open(const std::string &path);
  void close();
  bool is_open() const;

protected:
  int_type underflow() override;

private:
  static constexpr std::size_t kBufferSize = 1 << 15;

  gzFile file_;
  char buffer_[kBufferSize];
};

class gzip_istream : public std::istream {
public:
  gzip_istream();
  explicit gzip_istream(const std::string &path);
  gzip_istream(const gzip_istream &) = delete;
  gzip_istream &operator=(const gzip_istream &) = delete;

  bool open(const std::string &path);
  void close();
  bool is_open() const;

private:
  gzip_istreambuf buffer_;
};

class gzip_ostream {
public:
  gzip_ostream();
  explicit gzip_ostream(const std::string &path,
                        int level = Z_DEFAULT_COMPRESSION);
  gzip_ostream(const gzip_ostream &) = delete;
  gzip_ostream &operator=(const gzip_ostream &) = delete;
  ~gzip_ostream();

  bool open(const std::string &path, int level = Z_DEFAULT_COMPRESSION);
  void write(const char *data, std::streamsize size);
  void put(char value);
  void close();
  bool is_open() const;

private:
  gzFile file_;
};

std::string gzip_compress_string(std::string input, int level);

// FASTQ block helpers.
uint32_t read_fastq_block(std::istream *input_stream, std::string *id_array,
                          std::string *read_array, std::string *quality_array,
                          const uint32_t &num_reads, const bool &fasta_flag);

void write_fastq_block(std::ofstream &output_stream, std::string *id_array,
                       std::string *read_array, std::string *quality_array,
                       const uint32_t &num_reads, const bool preserve_quality,
                       const int &num_thr, const bool &gzip_flag,
                       const int &compression_level, const bool use_crlf);

// ID stream helpers.
void compress_id_block(const char *output_path, std::string *id_array,
                       const uint32_t &num_ids, const int &compression_level,
                       bool pack_only = false);

void decompress_id_block(const char *input_path, std::string *id_array,
                         const uint32_t &num_ids, bool pack_only = false);

// Quality helpers.
void quantize_quality(std::string *quality_array, const uint32_t &num_lines,
                      char *quantization_table);

// Reliability helpers.
void safe_bsc_decompress(const std::string &input_path,
                         const std::string &output_path);
void safe_bsc_str_array_decompress(const std::string &input_path,
                                   std::string *string_array,
                                   uint32_t num_strings,
                                   uint32_t *string_lengths);

void quantize_quality_qvz(std::string *quality_array, const uint32_t &num_lines,
                          uint32_t *str_len_array, double qv_ratio);

void generate_illumina_binning_table(char *illumina_binning_table);

void generate_binary_binning_table(char *binary_binning_table,
                                   const unsigned int thr,
                                   const unsigned int high,
                                   const unsigned int low);

// Paired-id helpers.
uint8_t find_id_pattern(const std::string &id_1, const std::string &id_2);

bool check_id_pattern(const std::string &id_1, const std::string &id_2,
                      const uint8_t paired_id_code);

void modify_id(std::string &id, const uint8_t paired_id_code);

// DNA packing helpers.
void write_dna_in_bits(const std::string &read, std::ofstream &fout);

void read_dna_from_bits(std::string &read, std::ifstream &fin);

void write_dnaN_in_bits(const std::string &read, std::ofstream &fout);

void read_dnaN_from_bits(std::string &read, std::ifstream &fin);

void reverse_complement(char *input_bases, char *output_bases,
                        const int readlen);

std::string reverse_complement(const std::string &input_bases,
                               const int readlen);

// Miscellaneous helpers.
void remove_CR_from_end(std::string &str);

size_t get_directory_size(const std::string &temp_dir);

// Temporary integer stream helpers.
void write_var_int64(const int64_t value, std::ofstream &output_stream);

int64_t read_var_int64(std::ifstream &input_stream);

} // namespace spring

#endif // SPRING_UTIL_H_
