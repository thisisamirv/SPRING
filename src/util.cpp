// Implements shared FASTQ I/O, DNA packing, identifier handling, quality
// quantization, and miscellaneous helpers used across Spring stages.

#include "util.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <vector>

#include "libbsc/bsc.h"
#include <cstring>
#include <libdeflate.h>
#include <zstd.h>

#include "omp.h"
#include "qvz/include/qvz.h"
#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <sys/stat.h>

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

thread_local std::string tl_plain_buffer;
thread_local std::string tl_compressed_buffer;

namespace {
struct LibdeflateDeleter {
  void operator()(libdeflate_compressor *c) const {
    if (c)
      libdeflate_free_compressor(c);
  }
};
} // namespace

thread_local std::unique_ptr<libdeflate_compressor, LibdeflateDeleter>
    tl_compressor;
thread_local int tl_compressor_level = -1;

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

void append_fastq_record(std::string &out, const std::string &id,
                         const std::string &read,
                         const std::string *quality_or_null,
                         const bool use_crlf, const bool fasta_mode) {
  const char *eol = use_crlf ? "\r\n" : "\n";
  out += id;
  out += eol;
  out += read;
  out += eol;
  if (!fasta_mode) {
    out += "+";
    out += eol;
    if (quality_or_null != nullptr) {
      out += *quality_or_null;
      out += eol;
    } else {
      out.append(read.size(), 'I');
      out += eol;
    }
  }
}

void append_fastq_records_range(std::string &output_buffer,
                                std::string *id_array, std::string *read_array,
                                const std::string *quality_or_null,
                                const uint64_t start_read_index,
                                const uint64_t end_read_index,
                                const bool use_crlf, const bool fasta_mode) {
  for (uint64_t read_index = start_read_index; read_index < end_read_index;
       read_index++) {
    append_fastq_record(
        output_buffer, id_array[read_index], read_array[read_index],
        quality_or_null == nullptr ? nullptr : &quality_or_null[read_index],
        use_crlf, fasta_mode);
  }
}

void write_gzip_fastq_block(std::ofstream &output_stream, std::string *id_array,
                            std::string *read_array,
                            const std::string *quality_or_null,
                            const uint32_t num_reads, const int num_thr,
                            const int gzip_level, const bool use_crlf,
                            const bool fasta_mode) {
  if (num_reads == 0)
    return;

  std::vector<std::string> gzip_compressed(static_cast<size_t>(num_thr));
  const std::vector<read_range> thread_ranges =
      compute_read_ranges(num_reads, num_thr);
#pragma omp parallel num_threads(num_thr)
  {
    const int tid = omp_get_thread_num();
    tl_plain_buffer.clear();
    const read_range &range = thread_ranges[static_cast<size_t>(tid)];
    append_fastq_records_range(tl_plain_buffer, id_array, read_array,
                               quality_or_null, range.start, range.end,
                               use_crlf, fasta_mode);
    gzip_compressed[tid] = gzip_compress_string(tl_plain_buffer, gzip_level);
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
    // Look for any space that is followed by '1' in id_1 and '2' in id_2,
    // with everything else being identical.
    for (size_t i = 0; i + 1 < len; ++i) {
      if (id_1[i] == ' ' && id_1[i + 1] == '1' && id_2[i + 1] == '2') {
        // Check if all other characters match
        bool mismatch = false;
        for (size_t j = 0; j < len; ++j) {
          if (j == i + 1)
            continue;
          if (id_1[j] != id_2[j]) {
            mismatch = true;
            break;
          }
        }
        if (!mismatch)
          return true;
      }
    }
    return false;
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

std::string gzip_compress_string(const std::string &input,
                                 const int gzip_level) {
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

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
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

  if (tl_compressor_level != gzip_level) {
    tl_compressor.reset(libdeflate_alloc_compressor(gzip_level));
    tl_compressor_level = gzip_level;
  }
  if (!tl_compressor) {
    throw std::runtime_error("Failed initializing libdeflate gzip compressor.");
  }

  std::string output;
  output.resize(
      libdeflate_gzip_compress_bound(tl_compressor.get(), input.size()));

  const size_t compressed_size =
      libdeflate_gzip_compress(tl_compressor.get(), input.data(), input.size(),
                               output.data(), output.size());

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
                       std::string *read_array,
                       const std::string *quality_array,
                       const uint32_t &num_reads, const int &num_thr,
                       const bool &gzip_flag, const bool &bgzf_flag,
                       const int &compression_level, const bool use_crlf,
                       const bool fasta_mode) {
  const std::string *quality_or_null = quality_array;
  if (!gzip_flag) {
    tl_plain_buffer.clear();
    append_fastq_records_range(tl_plain_buffer, id_array, read_array,
                               quality_or_null, 0, num_reads, use_crlf,
                               fasta_mode);
    output_stream.write(tl_plain_buffer.data(),
                        static_cast<std::streamsize>(tl_plain_buffer.size()));
  } else if (bgzf_flag) {
    write_bgzf_fastq_block(output_stream, id_array, read_array, quality_or_null,
                           num_reads, num_thr, compression_level, use_crlf,
                           fasta_mode);
  } else {
    // Compression level is 1-9 for CLI; pass to gzip unchanged (clamped).
    const int mapped_gzip_level = std::max(1, std::min(9, compression_level));
    write_gzip_fastq_block(output_stream, id_array, read_array, quality_or_null,
                           num_reads, num_thr, mapped_gzip_level, use_crlf,
                           fasta_mode);
  }
}

void write_bgzf_fastq_block(std::ofstream &output_stream, std::string *id_array,
                            std::string *read_array,
                            const std::string *quality_array,
                            const uint32_t &num_reads, const int &num_thr,
                            const int &compression_level, const bool use_crlf,
                            const bool fasta_mode) {
  if (num_reads == 0)
    return;

  std::vector<std::string> compressed_parts(num_thr);

#pragma omp parallel num_threads(num_thr)
  {
    int thread_id = omp_get_thread_num();
    uint32_t start_idx = (uint64_t(num_reads) * thread_id) / num_thr;
    uint32_t end_idx = (uint64_t(num_reads) * (thread_id + 1)) / num_thr;

    tl_plain_buffer.clear();
    append_fastq_records_range(tl_plain_buffer, id_array, read_array,
                               quality_array, start_idx, end_idx, use_crlf,
                               fasta_mode);

    if (!tl_plain_buffer.empty()) {
      // BGZF uses compression_level up to 12 in some variants, but libdeflate
      // usually 1-12. CLAMP to safe range.
      int effective_level = std::clamp(compression_level, 1, 12);
      if (tl_compressor_level != effective_level) {
        tl_compressor.reset(libdeflate_alloc_compressor(effective_level));
        tl_compressor_level = effective_level;
      }
      if (!tl_compressor) {
        throw std::runtime_error("Failed initializing BGZF compressor.");
      }
      const char *ptr = tl_plain_buffer.data();
      size_t remaining = tl_plain_buffer.size();
      std::vector<char> compressed_buffer(65536 + 1024);

      while (remaining > 0) {
        size_t block_size = std::min<size_t>(remaining, 65536);
        size_t cp_size = libdeflate_deflate_compress(
            tl_compressor.get(), ptr, block_size, compressed_buffer.data(),
            compressed_buffer.size());

        // Header (10 bytes) + Extra (8 bytes)
        unsigned char header[18] = {0x1f, 0x8b, 0x08, 0x04, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0xff, 0x06, 0x00,
                                    0x42, 0x43, 0x02, 0x00, 0x00, 0x00};
        uint16_t bsiz = static_cast<uint16_t>(18 + cp_size + 8 - 1);
        std::memcpy(&header[16], &bsiz, 2);

        compressed_parts[thread_id].append(
            reinterpret_cast<const char *>(header), 18);
        compressed_parts[thread_id].append(compressed_buffer.data(), cp_size);

        // For BGZF, each block is a completely independent gzip member with its
        // own CRC and ISIZE. The CRC is calculated only over the current 64KB
        // block rather than the entire thread chunk.
        uint32_t crc = libdeflate_crc32(0, ptr, block_size);
        uint32_t isize = static_cast<uint32_t>(block_size);
        compressed_parts[thread_id].append(reinterpret_cast<const char *>(&crc),
                                           4);
        compressed_parts[thread_id].append(
            reinterpret_cast<const char *>(&isize), 4);

        ptr += block_size;
        remaining -= block_size;
      }
    }
  }

  for (const auto &part : compressed_parts) {
    output_stream.write(part.data(), part.size());
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
  if (id.empty())
    return;
  if (paired_id_code == 2)
    return;
  else if (paired_id_code == 1) {
    if (id.back() == '1')
      id.back() = '2';
    return;
  } else if (paired_id_code == 3) {
    // Find the space followed by '1' and change it to '2'
    for (size_t i = 0; i + 1 < id.size(); ++i) {
      if (id[i] == ' ' && id[i + 1] == '1') {
        id[i + 1] = '2';
        return;
      }
    }
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

  if (!fs::exists(p, ec))
    return 0;

  for (const auto &entry : fs::recursive_directory_iterator(p, ec)) {
    if (ec)
      break;
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
    throw std::runtime_error(
        "Compressed string array is too small to be valid: " + input_path);
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

void write_bool(std::ostream &out, bool value) {
  uint8_t byte = value ? 1 : 0;
  out.write(byte_ptr(&byte), sizeof(uint8_t));
}

bool read_bool(std::istream &in) {
  uint8_t byte = 0;
  in.read(byte_ptr(&byte), sizeof(uint8_t));
  return byte != 0;
}

void write_string(std::ostream &out, const std::string &s) {
  uint32_t len = static_cast<uint32_t>(s.length());
  out.write(byte_ptr(&len), sizeof(uint32_t));
  if (len > 0) {
    out.write(s.data(), len);
  }
}

std::string read_string(std::istream &in) {
  uint32_t len = 0;
  in.read(byte_ptr(&len), sizeof(uint32_t));
  if (len == 0) {
    return std::string();
  }
  std::string s(len, '\0');
  in.read(&s[0], len);
  return s;
}

void write_compression_params(std::ostream &out, const compression_params &cp) {
  write_bool(out, cp.paired_end);
  write_bool(out, cp.preserve_order);
  write_bool(out, cp.preserve_quality);
  write_bool(out, cp.preserve_id);
  write_bool(out, cp.long_flag);
  write_bool(out, cp.qvz_flag);
  write_bool(out, cp.ill_bin_flag);
  write_bool(out, cp.bin_thr_flag);
  out.write(byte_ptr(&cp.qvz_ratio), sizeof(double));
  out.write(byte_ptr(&cp.bin_thr_thr), sizeof(unsigned int));
  out.write(byte_ptr(&cp.bin_thr_high), sizeof(unsigned int));
  out.write(byte_ptr(&cp.bin_thr_low), sizeof(unsigned int));
  out.write(byte_ptr(&cp.num_reads), sizeof(uint32_t));
  out.write(byte_ptr(&cp.num_reads_clean[0]), sizeof(uint32_t));
  out.write(byte_ptr(&cp.num_reads_clean[1]), sizeof(uint32_t));
  out.write(byte_ptr(&cp.max_readlen), sizeof(uint32_t));
  out.write(byte_ptr(&cp.paired_id_code), sizeof(uint8_t));
  write_bool(out, cp.paired_id_match);
  out.write(byte_ptr(&cp.num_reads_per_block), sizeof(int));
  out.write(byte_ptr(&cp.num_reads_per_block_long), sizeof(int));
  out.write(byte_ptr(&cp.num_thr), sizeof(int));
  out.write(byte_ptr(&cp.compression_level), sizeof(int));
  out.write(reinterpret_cast<const char *>(cp.file_len_seq_thr),
            sizeof(uint64_t) * compression_params::kFileLenThrSize);
  out.write(reinterpret_cast<const char *>(cp.file_len_id_thr),
            sizeof(uint64_t) * compression_params::kFileLenThrSize);
  write_bool(out, cp.use_crlf);
  write_string(out, cp.input_filename_1);
  write_string(out, cp.input_filename_2);
  write_string(out, cp.note);
  write_bool(out, cp.fasta_mode);

  // Serialize enhanced gzip/BGZF metadata
  write_bool(out, cp.input_1_was_gzipped);
  write_bool(out, cp.input_2_was_gzipped);
  out.write(byte_ptr(&cp.input_1_gzip_flg), sizeof(uint8_t));
  out.write(byte_ptr(&cp.input_2_gzip_flg), sizeof(uint8_t));
  out.write(byte_ptr(&cp.input_1_gzip_mtime), sizeof(uint32_t));
  out.write(byte_ptr(&cp.input_2_gzip_mtime), sizeof(uint32_t));
  out.write(byte_ptr(&cp.input_1_gzip_xfl), sizeof(uint8_t));
  out.write(byte_ptr(&cp.input_2_gzip_xfl), sizeof(uint8_t));
  out.write(byte_ptr(&cp.input_1_gzip_os), sizeof(uint8_t));
  out.write(byte_ptr(&cp.input_2_gzip_os), sizeof(uint8_t));
  write_string(out, cp.input_1_gzip_name);
  write_string(out, cp.input_2_gzip_name);
  write_bool(out, cp.input_1_is_bgzf);
  write_bool(out, cp.input_2_is_bgzf);
  out.write(byte_ptr(&cp.input_1_bgzf_block_size), sizeof(uint16_t));
  out.write(byte_ptr(&cp.input_2_bgzf_block_size), sizeof(uint16_t));
  out.write(byte_ptr(&cp.input_1_gzip_uncompressed_size), sizeof(uint64_t));
  out.write(byte_ptr(&cp.input_2_gzip_uncompressed_size), sizeof(uint64_t));
  out.write(byte_ptr(&cp.input_1_gzip_compressed_size), sizeof(uint64_t));
  out.write(byte_ptr(&cp.input_2_gzip_compressed_size), sizeof(uint64_t));
  out.write(byte_ptr(&cp.input_1_gzip_member_count), sizeof(uint32_t));
  out.write(byte_ptr(&cp.input_2_gzip_member_count), sizeof(uint32_t));
}

void read_compression_params(std::istream &in, compression_params &cp) {
  cp.paired_end = read_bool(in);
  cp.preserve_order = read_bool(in);
  cp.preserve_quality = read_bool(in);
  cp.preserve_id = read_bool(in);
  cp.long_flag = read_bool(in);
  cp.qvz_flag = read_bool(in);
  cp.ill_bin_flag = read_bool(in);
  cp.bin_thr_flag = read_bool(in);
  in.read(byte_ptr(&cp.qvz_ratio), sizeof(double));
  in.read(byte_ptr(&cp.bin_thr_thr), sizeof(unsigned int));
  in.read(byte_ptr(&cp.bin_thr_high), sizeof(unsigned int));
  in.read(byte_ptr(&cp.bin_thr_low), sizeof(unsigned int));
  in.read(byte_ptr(&cp.num_reads), sizeof(uint32_t));
  in.read(byte_ptr(&cp.num_reads_clean[0]), sizeof(uint32_t));
  in.read(byte_ptr(&cp.num_reads_clean[1]), sizeof(uint32_t));
  in.read(byte_ptr(&cp.max_readlen), sizeof(uint32_t));
  in.read(byte_ptr(&cp.paired_id_code), sizeof(uint8_t));
  cp.paired_id_match = read_bool(in);
  in.read(byte_ptr(&cp.num_reads_per_block), sizeof(int));
  in.read(byte_ptr(&cp.num_reads_per_block_long), sizeof(int));
  in.read(byte_ptr(&cp.num_thr), sizeof(int));
  in.read(byte_ptr(&cp.compression_level), sizeof(int));
  in.read(reinterpret_cast<char *>(cp.file_len_seq_thr),
          sizeof(uint64_t) * compression_params::kFileLenThrSize);
  in.read(reinterpret_cast<char *>(cp.file_len_id_thr),
          sizeof(uint64_t) * compression_params::kFileLenThrSize);
  cp.use_crlf = read_bool(in);
  cp.input_filename_1 = read_string(in);
  cp.input_filename_2 = read_string(in);
  cp.note = read_string(in);
  cp.fasta_mode = read_bool(in);

  // Deserialize enhanced gzip/BGZF metadata
  cp.input_1_was_gzipped = read_bool(in);
  cp.input_2_was_gzipped = read_bool(in);
  in.read(byte_ptr(&cp.input_1_gzip_flg), sizeof(uint8_t));
  in.read(byte_ptr(&cp.input_2_gzip_flg), sizeof(uint8_t));
  in.read(byte_ptr(&cp.input_1_gzip_mtime), sizeof(uint32_t));
  in.read(byte_ptr(&cp.input_2_gzip_mtime), sizeof(uint32_t));
  in.read(byte_ptr(&cp.input_1_gzip_xfl), sizeof(uint8_t));
  in.read(byte_ptr(&cp.input_2_gzip_xfl), sizeof(uint8_t));
  in.read(byte_ptr(&cp.input_1_gzip_os), sizeof(uint8_t));
  in.read(byte_ptr(&cp.input_2_gzip_os), sizeof(uint8_t));
  cp.input_1_gzip_name = read_string(in);
  cp.input_2_gzip_name = read_string(in);
  cp.input_1_is_bgzf = read_bool(in);
  cp.input_2_is_bgzf = read_bool(in);
  in.read(byte_ptr(&cp.input_1_bgzf_block_size), sizeof(uint16_t));
  in.read(byte_ptr(&cp.input_2_bgzf_block_size), sizeof(uint16_t));
  in.read(byte_ptr(&cp.input_1_gzip_uncompressed_size), sizeof(uint64_t));
  in.read(byte_ptr(&cp.input_2_gzip_uncompressed_size), sizeof(uint64_t));
  in.read(byte_ptr(&cp.input_1_gzip_compressed_size), sizeof(uint64_t));
  in.read(byte_ptr(&cp.input_2_gzip_compressed_size), sizeof(uint64_t));
  in.read(byte_ptr(&cp.input_1_gzip_member_count), sizeof(uint32_t));
  in.read(byte_ptr(&cp.input_2_gzip_member_count), sizeof(uint32_t));
}

void extract_gzip_detailed_info(const std::string &path, bool &is_gzipped,
                                uint8_t &flg, uint32_t &mtime, uint8_t &xfl,
                                uint8_t &os, std::string &name, bool &is_bgzf,
                                uint16_t &bgzf_block_size,
                                uint64_t &uncompressed_size,
                                uint64_t &compressed_size,
                                uint32_t &member_count) {
  is_gzipped = false;
  is_bgzf = false;
  flg = 0;
  mtime = 0;
  xfl = 0;
  os = 0;
  name.clear();
  bgzf_block_size = 0;
  uncompressed_size = 0;
  compressed_size = 0;
  member_count = 0;

  std::ifstream fin(path, std::ios::binary);
  if (!fin) {
    return;
  }

  fin.seekg(0, std::ios::end);
  compressed_size = fin.tellg();
  fin.seekg(0, std::ios::beg);

  while (true) {
    const std::streampos member_start = fin.tellg();
    unsigned char header[10];
    if (!fin.read(reinterpret_cast<char *>(header), 10))
      break;

    if (header[0] != 0x1f || header[1] != 0x8b) {
      if (member_count == 0)
        return;
      break;
    }

    if (member_count == 0) {
      is_gzipped = true;
      flg = header[3];
      std::memcpy(&mtime, &header[4], 4);
      xfl = header[8];
      os = header[9];
    }

    member_count++;
    const uint8_t current_flg = header[3];
    bool current_is_bgzf = false;
    uint16_t current_bsiz = 0;

    // FEXTRA
    if (current_flg & 0x04) {
      uint16_t xlen;
      fin.read(reinterpret_cast<char *>(&xlen), 2);
      const std::streampos extra_start = fin.tellg();
      while (fin.tellg() - extra_start < xlen) {
        char si1, si2;
        uint16_t slen;
        fin.read(&si1, 1);
        fin.read(&si2, 1);
        fin.read(reinterpret_cast<char *>(&slen), 2);
        if (si1 == 'B' && si2 == 'C' && slen == 2) {
          fin.read(reinterpret_cast<char *>(&current_bsiz), 2);
          current_is_bgzf = true;
          if (member_count == 1) {
            is_bgzf = true;
            bgzf_block_size = current_bsiz + 1;
          }
        } else {
          fin.seekg(slen, std::ios::cur);
        }
      }
    }

    // FNAME
    if (current_flg & 0x08) {
      std::string current_name;
      char c;
      while (fin.read(&c, 1) && c != '\0') {
        current_name += c;
      }
      if (member_count == 1)
        name = current_name;
    }

    // FCOMMENT
    if (current_flg & 0x10) {
      char c;
      while (fin.read(&c, 1) && c != '\0')
        ;
    }

    // FHCRC
    if (current_flg & 0x02) {
      fin.seekg(2, std::ios::cur);
    }

    if (current_is_bgzf) {
      // BGZF allows us to skip fast!
      // Block size including header and footer is BSIZ + 1
      fin.seekg(member_start + static_cast<std::streamoff>(current_bsiz - 7),
                std::ios::beg);
    } else {
      // Non-BGZF: We can't easily find the next member without decompressing.
      // For now, we'll try to get the footer of the last member if possible,
      // or just assume it's a single-member file if we can't skip.
      // Let's at least try to read the footer of the FIRST member if it's the
      // only one. But for multi-member non-BGZF, this is hard with just
      // std::ifstream. We'll skip to the end of the file and read the last 8
      // bytes for uncompressed size. This is what `gzip -l` does for single
      // members.
      break;
    }

    // Read CRC32 and ISIZE (8 bytes)
    uint32_t isize;
    fin.seekg(4, std::ios::cur); // skip CRC32
    fin.read(reinterpret_cast<char *>(&isize), 4);
    uncompressed_size += isize;

    // Check if there is more data
    if (fin.peek() == EOF)
      break;
  }

  if (uncompressed_size == 0 && member_count > 0) {
    // Fallback for single-member non-BGZF
    fin.clear();
    fin.seekg(-4, std::ios::end);
    uint32_t isize;
    fin.read(reinterpret_cast<char *>(&isize), 4);
    uncompressed_size = isize;
  }
}

std::string shell_quote(const std::string &value) {
#ifdef _WIN32
  // On Windows, use generic_string() (forward slashes) for relative/absolute
  // paths passed to system(). cmd.exe and Windows APIs usually handle them.
  std::string quoted = "\"";
  for (const char character : value) {
    if (character == '"') {
      quoted += "\\\"";
    } else {
      quoted += character;
    }
  }
  quoted += '"';
  return quoted;
#else
  std::string quoted = "'";
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\"'\"'";
    } else {
      quoted += character;
    }
  }
  quoted += "'";
  return quoted;
#endif
}

std::string shell_path(const std::string &value) {
  return std::filesystem::path(value).generic_string();
}

bool has_suffix(const std::string &value, const std::string &suffix) {
  if (suffix.size() > value.size())
    return false;
  return value.ends_with(suffix);
}

int parse_int_or_throw(const std::string &value, const char *error_message) {
  try {
    size_t parsed_chars = 0;
    int parsed_value = std::stoi(value, &parsed_chars);
    if (parsed_chars != value.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return parsed_value;
  } catch (const std::exception &) {
    throw std::runtime_error(error_message);
  }
}

double parse_double_or_throw(const std::string &value,
                             const char *error_message) {
  try {
    size_t parsed_chars = 0;
    double parsed_value = std::stod(value, &parsed_chars);
    if (parsed_chars != value.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return parsed_value;
  } catch (const std::exception &) {
    throw std::runtime_error(error_message);
  }
}

uint64_t parse_uint64_or_throw(const std::string &value,
                               const char *error_message) {
  try {
    size_t parsed_chars = 0;
    uint64_t parsed_value = std::stoull(value, &parsed_chars);
    if (parsed_chars != value.size()) {
      throw std::invalid_argument("trailing characters");
    }
    return parsed_value;
  } catch (const std::exception &) {
    throw std::runtime_error(error_message);
  }
}

void create_tar_archive(const std::string &archive_path,
                        const std::string &source_dir) {
  struct archive *a;
  struct archive_entry *entry;
  struct stat st;
  char buff[65536];
  int len;
  int fd;

  a = archive_write_new();
  archive_write_set_format_pax_restricted(a);
  if (archive_write_open_filename(a, archive_path.c_str()) != ARCHIVE_OK) {
    throw std::runtime_error("Failed to open archive for writing: " +
                             std::string(archive_error_string(a)));
  }

  std::filesystem::path root(source_dir);
  for (const auto &dir_entry :
       std::filesystem::recursive_directory_iterator(root)) {
    if (!dir_entry.is_regular_file())
      continue;

    const std::string full_path = dir_entry.path().string();
    const std::string rel_path =
        std::filesystem::relative(dir_entry.path(), root).string();

    stat(full_path.c_str(), &st);
    entry = archive_entry_new();
    archive_entry_set_pathname(entry, rel_path.c_str());
    archive_entry_set_size(entry, st.st_size);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_write_header(a, entry);

    fd = open(full_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      len = read(fd, buff, sizeof(buff));
      while (len > 0) {
        archive_write_data(a, buff, len);
        len = read(fd, buff, sizeof(buff));
      }
      close(fd);
    }
    archive_entry_free(entry);
  }

  archive_write_close(a);
  archive_write_free(a);
}

void extract_tar_archive(const std::string &archive_path,
                         const std::string &target_dir) {
  struct archive *a;
  struct archive *ext;
  struct archive_entry *entry;
  int flags;
  int r;

  flags = ARCHIVE_EXTRACT_TIME;
  flags |= ARCHIVE_EXTRACT_PERM;
  flags |= ARCHIVE_EXTRACT_ACL;
  flags |= ARCHIVE_EXTRACT_FFLAGS;

  a = archive_read_new();
  archive_read_support_format_all(a);
  archive_read_support_filter_all(a);
  ext = archive_write_disk_new();
  archive_write_disk_set_options(ext, flags);
  archive_write_disk_set_standard_lookup(ext);

  r = archive_read_open_filename(a, archive_path.c_str(), 10240);
  if (r != ARCHIVE_OK) {
    throw std::runtime_error("Failed to open archive for reading: " +
                             std::string(archive_error_string(a)));
  }

  std::filesystem::create_directories(target_dir);

  for (;;) {
    r = archive_read_next_header(a, &entry);
    if (r == ARCHIVE_EOF)
      break;
    if (r < ARCHIVE_OK) {
      // Warning or error
    }
    if (r < ARCHIVE_WARN) {
      throw std::runtime_error("Error reading archive header: " +
                               std::string(archive_error_string(a)));
    }

    std::filesystem::path dest_path =
        std::filesystem::path(target_dir) / archive_entry_pathname(entry);
    archive_entry_set_pathname(entry, dest_path.string().c_str());

    r = archive_write_header(ext, entry);
    if (r < ARCHIVE_OK) {
      // Warning or error
    } else if (archive_entry_size(entry) > 0) {
      const void *buff;
      size_t size;
      la_int64_t offset;
      while (true) {
        r = archive_read_data_block(a, &buff, &size, &offset);
        if (r == ARCHIVE_EOF)
          break;
        if (r < ARCHIVE_OK)
          throw std::runtime_error("Error reading archive data: " +
                                   std::string(archive_error_string(a)));
        r = archive_write_data_block(ext, buff, size, offset);
        if (r < ARCHIVE_OK)
          throw std::runtime_error("Error writing disk data: " +
                                   std::string(archive_error_string(ext)));
      }
    }
    r = archive_write_finish_entry(ext);
    if (r < ARCHIVE_OK)
      throw std::runtime_error("Error finishing disk entry: " +
                               std::string(archive_error_string(ext)));
  }

  archive_read_close(a);
  archive_read_free(a);
  archive_write_close(ext);
  archive_write_free(ext);
}

} // namespace spring
