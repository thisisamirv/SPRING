// Implements shared FASTQ I/O, DNA packing, identifier handling, quality
// quantization, and miscellaneous helpers used across Spring stages.

#include "util.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <vector>

#include <cstring>
#include <libdeflate.h>
#include <zstd.h>
#include "libbsc/bsc.h"

#include "omp.h"
#include "qvz/include/qvz.h"

namespace spring {

namespace {

const char *const kInvalidFastqError =
    "Invalid FASTQ(A) file. Number of lines not multiple of 4(2)";

constexpr std::streamsize kGzipChunkSize = 1 << 15;

struct read_range {
  uint64_t start;
  uint64_t end;
};

std::vector<read_range> compute_read_ranges(uint32_t num_reads, int num_thr);

std::runtime_error gzip_runtime_error(gzFile file_handle,
                                      const std::string &prefix) {
  int error_code = Z_OK;
  const char *message = gzerror(file_handle, &error_code);
  if (message == nullptr || error_code == Z_OK) {
    return std::runtime_error(prefix);
  }
  return std::runtime_error(prefix + ": " + message);
}

std::string gzip_mode_string(const char mode, const int level) {
  if (level == Z_DEFAULT_COMPRESSION) {
    return std::string() + mode + 'b';
  }
  if (level < 0 || level > 9) {
    throw std::runtime_error("gzip level must be between 0 and 9.");
  }
  return std::string() + mode + 'b' + static_cast<char>('0' + level);
}

void write_gzip_data(gzFile file_handle, const char *data,
                     std::streamsize size) {
  std::streamsize written_total = 0;
  while (written_total < size) {
    const std::streamsize chunk_size =
        std::min<std::streamsize>(size - written_total, kGzipChunkSize);
    const int written = gzwrite(file_handle, data + written_total,
                                static_cast<unsigned int>(chunk_size));
    if (written == 0) {
      throw gzip_runtime_error(file_handle, "Failed writing gzip stream");
    }
    written_total += written;
  }
}

const std::array<uint8_t, 128> &dna_to_int_lookup() {
  static const std::array<uint8_t, 128> lookup = []() {
    std::array<uint8_t, 128> table = {};
    table[(uint8_t)'A'] = 0;
    table[(uint8_t)'C'] = 2;
    table[(uint8_t)'G'] = 1;
    table[(uint8_t)'T'] = 3;
    return table;
  }();
  return lookup;
}

const std::array<uint8_t, 128> &dna_n_to_int_lookup() {
  static const std::array<uint8_t, 128> lookup = []() {
    std::array<uint8_t, 128> table = {};
    table[(uint8_t)'A'] = 0;
    table[(uint8_t)'C'] = 2;
    table[(uint8_t)'G'] = 1;
    table[(uint8_t)'T'] = 3;
    table[(uint8_t)'N'] = 4;
    return table;
  }();
  return lookup;
}

const std::array<char, 4> &int_to_dna_lookup() {
  static const std::array<char, 4> lookup = {'A', 'G', 'C', 'T'};
  return lookup;
}

const std::array<char, 5> &int_to_dna_n_lookup() {
  static const std::array<char, 5> lookup = {'A', 'G', 'C', 'T', 'N'};
  return lookup;
}

void write_fastq_record(std::ostream &out, const std::string &id,
                        const std::string &read,
                        const std::string *quality_or_null,
                        const bool use_crlf) {
  const char *eol = use_crlf ? "\r\n" : "\n";
  out << id << eol;
  out << read << eol;
  if (quality_or_null != nullptr) {
    out << "+" << eol;
    out << *quality_or_null << eol;
  }
}

void write_fastq_records_range(std::ostream &output_stream,
                               std::string *id_array, std::string *read_array,
                               const std::string *quality_or_null,
                               const uint64_t start_read_index,
                               const uint64_t end_read_index,
                               const bool use_crlf) {
  for (uint64_t read_index = start_read_index; read_index < end_read_index;
       read_index++) {
    write_fastq_record(
        output_stream, id_array[read_index], read_array[read_index],
        quality_or_null == nullptr ? nullptr : &quality_or_null[read_index],
        use_crlf);
  }
}

void write_gzip_fastq_block(std::ofstream &output_stream, std::string *id_array,
                            std::string *read_array,
                            const std::string *quality_or_null,
                            const uint32_t num_reads, const int num_thr,
                            const int gzip_level, const bool use_crlf) {
  if (num_reads == 0)
    return;

  std::vector<std::string> gzip_compressed(static_cast<size_t>(num_thr));
  const std::vector<read_range> thread_ranges =
      compute_read_ranges(num_reads, num_thr);
#pragma omp parallel num_threads(num_thr)
  {
    const int tid = omp_get_thread_num();
    std::ostringstream plain_output;
    const read_range &range = thread_ranges[static_cast<size_t>(tid)];
    write_fastq_records_range(plain_output, id_array, read_array,
                              quality_or_null, range.start, range.end,
                              use_crlf);
    gzip_compressed[tid] = gzip_compress_string(plain_output.str(), gzip_level);
  }

  for (int thread_index = 0; thread_index < num_thr; thread_index++) {
    output_stream.write(gzip_compressed[thread_index].data(),
                        gzip_compressed[thread_index].size());
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
    range.end =
        std::min<uint64_t>(range.start + num_reads_per_thread, num_reads);
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
    return false;
  }
}

template <size_t BufferSize>
void write_encoded_read(const std::string &read, std::ofstream &fout,
                        const uint8_t *dna_to_int, const uint8_t bits_per_base,
                        const uint8_t bases_per_byte) {
  uint8_t bitarray[BufferSize];
  uint8_t pos_in_bitarray = 0;
  const uint16_t readlen = read.size();
  const int bases_per_byte_count = bases_per_byte;
  fout.write(byte_ptr(&readlen), sizeof(uint16_t));

  const int full_groups = readlen / bases_per_byte;
  for (int group_index = 0; group_index < full_groups; ++group_index) {
    bitarray[pos_in_bitarray] = 0;
    for (int base_index = 0; base_index < bases_per_byte_count; ++base_index)
      bitarray[pos_in_bitarray] |=
          dna_to_int[static_cast<uint8_t>(
              read[bases_per_byte * group_index + base_index])]
          << (bits_per_base * base_index);
    ++pos_in_bitarray;
  }

  const int trailing_bases = readlen % bases_per_byte;
  if (trailing_bases != 0) {
    const int group_index = full_groups;
    bitarray[pos_in_bitarray] = 0;
    for (int base_index = 0; base_index < trailing_bases; ++base_index)
      bitarray[pos_in_bitarray] |=
          dna_to_int[static_cast<uint8_t>(
              read[bases_per_byte * group_index + base_index])]
          << (bits_per_base * base_index);
    ++pos_in_bitarray;
  }

  fout.write(byte_ptr(&bitarray[0]), pos_in_bitarray);
  if (!fout.good()) {
    throw std::runtime_error("Failed writing encoded read to binary stream.");
  }
}

template <size_t BufferSize>
void read_encoded_read(std::string &read, std::ifstream &fin,
                       const char *int_to_dna, const uint8_t bit_mask,
                       const uint8_t bits_per_base,
                       const uint8_t bases_per_byte) {
  uint16_t readlen;
  uint8_t bitarray[BufferSize];
  if (!fin.read(byte_ptr(&readlen), sizeof(uint16_t))) {
    if (fin.eof())
      return; // Graceful EOF
    throw std::runtime_error("Failed reading readlen from binary stream.");
  }
  read.resize(readlen);

  const uint16_t num_bytes_to_read =
      ((uint32_t)readlen + bases_per_byte - 1) / bases_per_byte;
  if (num_bytes_to_read > BufferSize) {
    throw std::runtime_error(
        "Corrupted binary read: record length (" + std::to_string(readlen) +
        ") exceeds buffer capacity (" +
        std::to_string(BufferSize * bases_per_byte) + ").");
  }
  if (!fin.read(byte_ptr(&bitarray[0]), num_bytes_to_read)) {
    throw std::runtime_error("Failed reading encoded data from binary stream.");
  }

  uint8_t pos_in_bitarray = 0;
  const int bases_per_byte_count = bases_per_byte;
  const int full_groups = readlen / bases_per_byte;
  for (int group_index = 0; group_index < full_groups; ++group_index) {
    for (int base_index = 0; base_index < bases_per_byte_count; ++base_index) {
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
    output_bases[index] =
        chartorevchar[static_cast<uint8_t>(input_bases[readlen - index - 1])];
}

} // namespace

gzip_istreambuf::gzip_istreambuf() : file_(nullptr) {
  setg(buffer_, buffer_, buffer_);
}

gzip_istreambuf::gzip_istreambuf(const std::string &path) : gzip_istreambuf() {
  open(path);
}

gzip_istreambuf::~gzip_istreambuf() { close(); }

bool gzip_istreambuf::open(const std::string &path) {
  close();
  file_ = gzopen(path.c_str(),
                 gzip_mode_string('r', Z_DEFAULT_COMPRESSION).c_str());
  if (file_ == nullptr) {
    return false;
  }
  gzbuffer(file_, static_cast<unsigned int>(kGzipChunkSize));
  setg(buffer_, buffer_, buffer_);
  return true;
}

void gzip_istreambuf::close() {
  if (file_ != nullptr) {
    gzclose(file_);
    file_ = nullptr;
  }
  setg(buffer_, buffer_, buffer_);
}

bool gzip_istreambuf::is_open() const { return file_ != nullptr; }

gzip_istreambuf::int_type gzip_istreambuf::underflow() {
  if (file_ == nullptr) {
    return traits_type::eof();
  }
  if (gptr() < egptr()) {
    return traits_type::to_int_type(*gptr());
  }

  const int bytes_read =
      gzread(file_, buffer_, static_cast<unsigned int>(sizeof(buffer_)));
  if (bytes_read < 0) {
    throw gzip_runtime_error(file_, "Failed reading gzip stream");
  }
  if (bytes_read == 0) {
    return traits_type::eof();
  }

  setg(buffer_, buffer_, buffer_ + bytes_read);
  return traits_type::to_int_type(*gptr());
}

gzip_istream::gzip_istream() : std::istream(&buffer_) {}

gzip_istream::gzip_istream(const std::string &path) : gzip_istream() {
  open(path);
}

bool gzip_istream::open(const std::string &path) {
  clear();
  if (!buffer_.open(path)) {
    setstate(std::ios::failbit);
    return false;
  }
  return true;
}

void gzip_istream::close() { buffer_.close(); }

bool gzip_istream::is_open() const { return buffer_.is_open(); }

gzip_ostream::gzip_ostream() : file_(nullptr) {}

gzip_ostream::gzip_ostream(const std::string &path, const int level)
    : gzip_ostream() {
  open(path, level);
}

gzip_ostream::~gzip_ostream() {
  if (file_ != nullptr) {
    try {
      close();
    } catch (...) {
    }
  }
}

bool gzip_ostream::open(const std::string &path, const int level) {
  close();
  file_ = gzopen(path.c_str(), gzip_mode_string('w', level).c_str());
  if (file_ == nullptr) {
    return false;
  }
  gzbuffer(file_, static_cast<unsigned int>(kGzipChunkSize));
  return true;
}

void gzip_ostream::write(const char *data, const std::streamsize size) {
  if (file_ == nullptr) {
    throw std::runtime_error("Gzip output stream is not open.");
  }
  write_gzip_data(file_, data, size);
}

void gzip_ostream::put(const char value) { write(&value, 1); }

void gzip_ostream::close() {
  if (file_ != nullptr) {
    if (gzclose(file_) != Z_OK) {
      file_ = nullptr;
      throw std::runtime_error("Failed closing gzip stream");
    }
    file_ = nullptr;
  }
}

bool gzip_ostream::is_open() const { return file_ != nullptr; }

std::string gzip_compress_string(std::string input, const int gzip_level) {
  if (gzip_level < 0 || gzip_level > 9) {
    throw std::runtime_error("gzip level must be between 0 and 9.");
  }
  // If running under Valgrind (or explicitly requested), avoid calling
  // libdeflate's optimized routines which may use instructions Valgrind
  // doesn't emulate. Fall back to zlib's deflate with gzip wrapper.
  const char *valgrind_env = getenv("VALGRIND");
  const char *running_on_valgrind = getenv("RUNNING_ON_VALGRIND");
  const char *force_zlib = getenv("SPRING_NO_LIBDEFLATE");

  if (valgrind_env != nullptr || running_on_valgrind != nullptr ||
      force_zlib != nullptr) {
    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));

    // Allocate output buffer with a safe bound.
    const size_t bound = compressBound(input.size());
    std::vector<unsigned char> outbuf(bound + 64);

    int ret = deflateInit2(&strm, gzip_level, Z_DEFLATED, 15 + 16, 8,
                           Z_DEFAULT_STRATEGY);
    if (ret != Z_OK)
      throw std::runtime_error("Failed initializing zlib gzip compressor.");

    // `input` is taken by value (owned) so `data()` returns a mutable
    // pointer; avoid const_cast and satisfy static analyzers.
    strm.next_in = reinterpret_cast<Bytef *>(input.data());
    strm.avail_in = static_cast<uInt>(input.size());
    strm.next_out = outbuf.data();
    strm.avail_out = static_cast<uInt>(outbuf.size());

    ret = deflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END) {
      deflateEnd(&strm);
      throw std::runtime_error("zlib gzip compression failed.");
    }

    const size_t out_size = strm.total_out;
    deflateEnd(&strm);
    return std::string(reinterpret_cast<char *>(outbuf.data()), out_size);
  }

  libdeflate_compressor *const compressor =
      libdeflate_alloc_compressor(gzip_level);
  if (compressor == nullptr) {
    throw std::runtime_error("Failed initializing gzip compressor.");
  }

  std::string output;
  output.resize(libdeflate_gzip_compress_bound(compressor, input.size()));

  const size_t compressed_size = libdeflate_gzip_compress(
      compressor, input.data(), input.size(), output.data(), output.size());
  libdeflate_free_compressor(compressor);

  if (compressed_size == 0) {
    throw std::runtime_error("Failed compressing gzip payload.");
  }

  output.resize(compressed_size);
  return output;
}

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
                       const int &compression_level, const bool use_crlf) {
  const std::string *quality_or_null =
      preserve_quality ? quality_array : nullptr;
  if (!gzip_flag) {
    write_fastq_records_range(output_stream, id_array, read_array,
                              quality_or_null, 0, num_reads, use_crlf);
  } else {
    // Compression level is 1-9 for CLI; pass to gzip unchanged (clamped).
    const int mapped_gzip_level = std::max(1, std::min(9, compression_level));
    write_gzip_fastq_block(output_stream, id_array, read_array, quality_or_null,
                           num_reads, num_thr, mapped_gzip_level, use_crlf);
  }
}

void compress_id_block(const char *output_path, std::string *id_array,
                       const uint32_t &num_ids, const int &compression_level,
                       bool pack_only) {
  (void)compression_level;
  if (num_ids == 0)
    return;

  std::vector<std::string> alpha_cols;
  std::vector<std::string> non_alpha_cols;
  std::vector<uint8_t> col_counts;
  col_counts.reserve(num_ids);

  for (uint32_t i = 0; i < num_ids; i++) {
    const std::string &id = id_array[i];
    uint8_t num_pairs = 0;
    size_t pos = 0;

    while (pos < id.size()) {
      if (num_pairs >= non_alpha_cols.size())
        non_alpha_cols.resize(num_pairs + 1);
      if (num_pairs >= alpha_cols.size())
        alpha_cols.resize(num_pairs + 1);

      // 1. Non-Alpha
      size_t end = pos;
      auto is_alnum = [](unsigned char c) { return std::isalnum(c); };
      while (end < id.size() && !is_alnum(static_cast<unsigned char>(id[end])))
        end++;
      non_alpha_cols[num_pairs].append(id.data() + pos, end - pos);
      non_alpha_cols[num_pairs].push_back('\0');
      pos = end;

      // 2. Alpha
      if (pos < id.size()) {
        end = pos;
        while (end < id.size() && is_alnum(static_cast<unsigned char>(id[end])))
          end++;
        alpha_cols[num_pairs].append(id.data() + pos, end - pos);
        alpha_cols[num_pairs].push_back('\0');
        pos = end;
      } else {
        alpha_cols[num_pairs].push_back('\0'); // Empty alpha
      }
      num_pairs++;
    }
    col_counts.push_back(num_pairs);
  }

  uint32_t num_nalpha = non_alpha_cols.size();
  uint32_t num_alpha = alpha_cols.size();
  uint32_t count_sz = col_counts.size();

  std::vector<std::string> new_alpha_cols;
  std::vector<uint8_t> alpha_fmts;
  new_alpha_cols.reserve(alpha_cols.size());
  alpha_fmts.reserve(alpha_cols.size());

  for (size_t c = 0; c < alpha_cols.size(); ++c) {
    const char *ptr = alpha_cols[c].data();
    const char *end_ptr = ptr + alpha_cols[c].size();
    bool is_numeric = true;

    const char *scan = ptr;
    while (scan < end_ptr) {
      size_t len = std::strlen(scan);
      if (len > 0) {
        if (len > 1 && scan[0] == '0') {
          is_numeric = false;
          break;
        }
        if (len > 9) {
          is_numeric = false;
          break;
        } // prevent uint32 overflow
        for (size_t i = 0; i < len; ++i) {
          if (!std::isdigit(static_cast<unsigned char>(scan[i]))) {
            is_numeric = false;
            break;
          }
        }
        if (!is_numeric)
          break;
      }
      scan += len + 1;
    }

    if (is_numeric) {
      std::vector<int32_t> deltas;
      uint32_t last_val = 0;
      scan = ptr;
      while (scan < end_ptr) {
        size_t len = std::strlen(scan);
        int32_t delta = INT32_MIN;
        if (len > 0) {
          uint32_t val = 0;
          for (size_t i = 0; i < len; ++i)
            val = val * 10 + (scan[i] - '0');
          delta = static_cast<int32_t>(val - last_val);
          last_val = val;
        }
        deltas.push_back(delta);
        scan += len + 1;
      }

      std::string deltas_str;
      deltas_str.resize(deltas.size() * sizeof(int32_t));
      size_t n = deltas.size();
      for (size_t i = 0; i < n; ++i) {
        uint32_t udelta;
        std::memcpy(&udelta, &deltas[i], sizeof(int32_t));
        deltas_str[i] = static_cast<char>(udelta & 0xFF);
        deltas_str[n + i] = static_cast<char>((udelta >> 8) & 0xFF);
        deltas_str[2 * n + i] = static_cast<char>((udelta >> 16) & 0xFF);
        deltas_str[3 * n + i] = static_cast<char>((udelta >> 24) & 0xFF);
      }

      alpha_fmts.push_back(1);
      new_alpha_cols.push_back(std::move(deltas_str));
    } else {
      alpha_fmts.push_back(0);
      new_alpha_cols.push_back(std::move(alpha_cols[c]));
    }
  }

  size_t est_size = 3 * sizeof(uint32_t) + count_sz;
  for (const auto &c : non_alpha_cols)
    est_size += sizeof(uint32_t) + c.size();
  for (const auto &c : new_alpha_cols)
    est_size += 1 + sizeof(uint32_t) + c.size();

  std::vector<uint8_t> buffer;
  buffer.reserve(est_size);

  auto write_u32 = [&buffer](uint32_t v) {
    const uint8_t *ptr = reinterpret_cast<const uint8_t *>(&v);
    buffer.insert(buffer.end(), ptr, ptr + sizeof(uint32_t));
  };
  auto write_str = [&buffer, &write_u32](const std::string &s) {
    write_u32(static_cast<uint32_t>(s.size()));
    const uint8_t *ptr = reinterpret_cast<const uint8_t *>(s.data());
    buffer.insert(buffer.end(), ptr, ptr + s.size());
  };

  write_u32(num_nalpha);
  write_u32(num_alpha);
  write_u32(count_sz);
  buffer.insert(buffer.end(), col_counts.begin(), col_counts.end());

  for (const auto &col : non_alpha_cols)
    write_str(col);
  for (size_t i = 0; i < alpha_fmts.size(); ++i) {
    buffer.push_back(alpha_fmts[i]);
    write_str(new_alpha_cols[i]);
  }

  if (pack_only) {
    std::ofstream fout(output_path, std::ios::binary);
    if (!fout)
      throw std::runtime_error("Failed to open ID pack output file.");
    fout.write(reinterpret_cast<const char *>(buffer.data()), buffer.size());
    return;
  }

  // ID compression switched from Zstd to BSC to restore the optimal 48MB ratio.
  const std::string temp_id_path = std::string(output_path) + ".tmp_id";
  std::ofstream id_out(temp_id_path, std::ios::binary);
  if (!id_out)
    throw std::runtime_error("Failed to open temporary ID file for writing.");
  id_out.write(reinterpret_cast<const char *>(buffer.data()), buffer.size());
  id_out.close();

  bsc::BSC_compress(temp_id_path.c_str(), output_path);
  remove(temp_id_path.c_str());
}

void decompress_id_block(const char *input_path, std::string *id_array,
                         const uint32_t &num_ids, bool pack_only) {
  if (num_ids == 0)
    return;

  std::string temp_id_path;
  if (pack_only) {
    temp_id_path = input_path;
  } else {
    // ID decompression switched back to BSC to match the restored encoder.
    temp_id_path = std::string(input_path) + ".tmp_id_dec";
    safe_bsc_decompress(input_path, temp_id_path);
  }

  std::ifstream id_in(temp_id_path, std::ios::binary | std::ios::ate);
  if (!id_in) {
    if (!pack_only)
      remove(temp_id_path.c_str());
    throw std::runtime_error("Failed to open temporary decompressed ID file.");
  }
  const size_t r_size = static_cast<size_t>(id_in.tellg());
  id_in.seekg(0, std::ios::beg);

  std::vector<uint8_t> buffer(r_size);
  if (!id_in.read(reinterpret_cast<char *>(buffer.data()), r_size)) {
    id_in.close();
    if (!pack_only)
      remove(temp_id_path.c_str());
    throw std::runtime_error("Failed to read decompressed ID block.");
  }
  id_in.close();
  if (!pack_only)
    remove(temp_id_path.c_str());

  const char *curr = reinterpret_cast<const char *>(buffer.data());
  const size_t d_size = r_size;
  const char *end = curr + d_size;

  auto read_u32 = [&]() {
    if (curr + sizeof(uint32_t) > end)
      throw std::runtime_error("Truncated C-SiT ID stream");
    uint32_t val;
    std::memcpy(&val, curr, sizeof(uint32_t));
    curr += sizeof(uint32_t);
    return val;
  };

  uint32_t num_nalpha = read_u32();
  uint32_t num_alpha = read_u32();
  uint32_t count_sz = read_u32();
  if (count_sz != num_ids)
    throw std::runtime_error("ID block mismatch in C-SiT count");

  if (curr + count_sz > end)
    throw std::runtime_error("Truncated C-SiT counts");
  const uint8_t *counts_ptr = reinterpret_cast<const uint8_t *>(curr);
  curr += count_sz;

  std::vector<const char *> nalpha_ptrs(num_nalpha);
  for (uint32_t i = 0; i < num_nalpha; ++i) {
    uint32_t len = read_u32();
    if (curr + len > end)
      throw std::runtime_error("Truncated C-SiT non-alpha column");
    nalpha_ptrs[i] = curr;
    curr += len;
  }

  std::vector<const char *> alpha_ptrs(num_alpha);
  std::vector<uint32_t> alpha_lens(num_alpha);
  std::vector<uint8_t> alpha_fmts(num_alpha);
  std::vector<uint32_t> alpha_last_vals(num_alpha, 0);
  std::vector<uint32_t> alpha_col_idx(num_alpha, 0);

  for (uint32_t i = 0; i < num_alpha; ++i) {
    if (curr + 1 > end)
      throw std::runtime_error("Truncated C-SiT alpha column fmt");
    alpha_fmts[i] = *curr++;
    uint32_t len = read_u32();
    if (curr + len > end)
      throw std::runtime_error("Truncated C-SiT alpha column");
    alpha_lens[i] = len;
    alpha_ptrs[i] = curr;
    curr += len;
  }

  for (uint32_t i = 0; i < num_ids; i++) {
    id_array[i].clear();
    uint8_t count = counts_ptr[i];
    for (uint8_t c = 0; c < count; ++c) {
      // read non-alpha
      const char *str = nalpha_ptrs[c];
      while (*str) {
        id_array[i].push_back(*str++);
      }
      nalpha_ptrs[c] = str + 1;

      // read alpha
      uint8_t fmt = alpha_fmts[c];
      if (fmt == 0) {
        str = alpha_ptrs[c];
        while (*str) {
          id_array[i].push_back(*str++);
        }
        alpha_ptrs[c] = str + 1;
      } else {
        uint32_t idx = alpha_col_idx[c]++;
        const uint8_t *raw = reinterpret_cast<const uint8_t *>(alpha_ptrs[c]);
        uint32_t n = alpha_lens[c] / 4;
        uint8_t b0 = raw[idx];
        uint8_t b1 = raw[n + idx];
        uint8_t b2 = raw[2 * n + idx];
        uint8_t b3 = raw[3 * n + idx];
        uint32_t udelta = static_cast<uint32_t>(b0) |
                          (static_cast<uint32_t>(b1) << 8) |
                          (static_cast<uint32_t>(b2) << 16) |
                          (static_cast<uint32_t>(b3) << 24);
        int32_t delta;
        std::memcpy(&delta, &udelta, sizeof(int32_t));

        if (delta != INT32_MIN) {
          uint32_t val = alpha_last_vals[c] + static_cast<uint32_t>(delta);
          alpha_last_vals[c] = val;
          id_array[i].append(std::to_string(val));
        }
      }
    }
  }
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
  const std::array<uint8_t, 128> &lookup = dna_to_int_lookup();
  write_encoded_read<128>(read, fout, lookup.data(), 2, 4);
}

void read_dna_from_bits(std::string &read, std::ifstream &fin) {
  const std::array<char, 4> &lookup = int_to_dna_lookup();
  read_encoded_read<128>(read, fin, lookup.data(), 3, 2, 4);
}

void write_dnaN_in_bits(const std::string &read, std::ofstream &fout) {
  const std::array<uint8_t, 128> &lookup = dna_n_to_int_lookup();
  write_encoded_read<256>(read, fout, lookup.data(), 4, 2);
}

void read_dnaN_from_bits(std::string &read, std::ifstream &fin) {
  const std::array<char, 5> &lookup = int_to_dna_n_lookup();
  read_encoded_read<256>(read, fin, lookup.data(), 15, 4, 2);
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
  namespace fs = std::filesystem;
  size_t size = 0;
  std::error_code ec;
  fs::path p{temp_dir};

  // On Windows, directory_iterator often fails if there's a trailing slash.
  if (p.has_relative_path() && !p.has_filename()) {
    p = p.parent_path();
  }

  if (!fs::exists(p, ec)) return 0;

  for (const auto &entry : fs::recursive_directory_iterator(p, ec)) {
    if (ec) break;
    std::error_code size_ec;
    if (fs::is_regular_file(entry.path(), size_ec)) {
      size += fs::file_size(entry.path(), size_ec);
    }
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


void safe_bsc_decompress(const std::string &input_path,
                         const std::string &output_path) {
  std::ifstream input(input_path, std::ios::binary | std::ios::ate);
  if (!input.is_open()) {
    throw std::runtime_error("Can't open compressed file for validation: " +
                             input_path);
  }

  const std::streampos file_size = input.tellg();
  if (file_size < 4) {
    throw std::runtime_error("Compressed file is too small to be valid: " +
                             input_path);
  }
  input.close();

  try {
    bsc::BSC_decompress(input_path.c_str(), output_path.c_str());
  } catch (const std::exception &e) {
    throw std::runtime_error("BSC decompression failed for " + input_path +
                             ": " + e.what());
  }
}

void safe_bsc_str_array_decompress(const std::string &input_path,
                                   std::string *string_array,
                                   uint32_t num_strings,
                                   uint32_t *string_lengths) {
  std::ifstream input(input_path, std::ios::binary | std::ios::ate);
  if (!input.is_open()) {
    throw std::runtime_error(
        "Can't open compressed string array for validation: " + input_path);
  }

  const std::streampos file_size = input.tellg();
  if (file_size < 4) {
    throw std::runtime_error("Compressed string array is too small to be valid: " +
                             input_path);
  }
  input.close();

  try {
    bsc::BSC_str_array_decompress(input_path.c_str(), string_array, num_strings,
                                  string_lengths);
  } catch (const std::exception &e) {
    throw std::runtime_error("BSC string array decompression failed for " +
                             input_path + ": " + e.what());
  }
}

} // namespace spring
