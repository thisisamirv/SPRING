#include "io_utils.h"
#include "bgzf.h"
#include "core_utils.h"
#include "libbsc/bsc.h"
#include "omp.h"
#include "parse_utils.h"
#include "qvz/include/qvz.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <libdeflate.h>
#include <stdexcept>
#include <vector>
#include <zstd.h>

namespace spring {

namespace {

const char *const kInvalidFastqError =
    "Invalid FASTQ(A) file. Number of lines not multiple of 4(2)";

constexpr std::streamsize kGzipChunkSize = 1 << 15;

#define MODE_FIXED 1
#define DISTORTION_MSE 2

struct read_range {
  uint64_t start;
  uint64_t end;
};

std::vector<read_range> compute_read_ranges(uint32_t num_reads, int num_thr) {
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

uint64_t zigzag_encode64(const int64_t value) {
  const uint64_t sign_mask = (value < 0) ? UINT64_MAX : 0U;
  return (static_cast<uint64_t>(value) << 1) ^ sign_mask;
}

int64_t zigzag_decode64(const uint64_t value) {
  return static_cast<int64_t>(value >> 1) ^ -static_cast<int64_t>(value & 1U);
}

} // namespace

gzip_istreambuf::gzip_istreambuf() : file_(nullptr), buffer_{} {}

gzip_istreambuf::gzip_istreambuf(const std::string &path)
    : file_(nullptr), buffer_{} {
  open(path);
}

gzip_istreambuf::~gzip_istreambuf() { close(); }

bool gzip_istreambuf::open(const std::string &path) {
  close();
  file_ = gzopen(path.c_str(), "rb");
  return file_ != nullptr;
}

void gzip_istreambuf::close() {
  if (file_) {
    gzclose(file_);
    file_ = nullptr;
  }
}

bool gzip_istreambuf::is_open() const { return file_ != nullptr; }

gzip_istreambuf::int_type gzip_istreambuf::underflow() {
  if (!is_open()) {
    return traits_type::eof();
  }

  const int bytes_read =
      gzread(file_, buffer_, static_cast<unsigned int>(kBufferSize));
  if (bytes_read <= 0) {
    return traits_type::eof();
  }

  setg(buffer_, buffer_, buffer_ + bytes_read);
  return traits_type::to_int_type(*gptr());
}

gzip_istream::gzip_istream() : std::istream(&buffer_) {}

gzip_istream::gzip_istream(const std::string &path)
    : std::istream(&buffer_), buffer_(path) {}

bool gzip_istream::open(const std::string &path) { return buffer_.open(path); }

void gzip_istream::close() { buffer_.close(); }

bool gzip_istream::is_open() const { return buffer_.is_open(); }

gzip_ostream::gzip_ostream() : file_(nullptr) {}

gzip_ostream::gzip_ostream(const std::string &path, int level)
    : file_(nullptr) {
  open(path, level);
}

gzip_ostream::~gzip_ostream() { close(); }

bool gzip_ostream::open(const std::string &path, int level) {
  close();
  std::string mode = "wb";
  if (level != Z_DEFAULT_COMPRESSION) {
    mode += std::to_string(level);
  }
  file_ = gzopen(path.c_str(), mode.c_str());
  return file_ != nullptr;
}

void gzip_ostream::write(const char *data, std::streamsize size) {
  if (!is_open())
    throw std::runtime_error("gzip_ostream is not open for writing.");
  write_gzip_data(file_, data, size);
}

void gzip_ostream::put(char value) {
  if (!is_open())
    throw std::runtime_error("gzip_ostream is not open for writing.");
  if (gzputc(file_, value) == -1) {
    throw gzip_runtime_error(file_,
                             "Failed writing single char to gzip stream");
  }
}

void gzip_ostream::close() {
  if (file_) {
    gzclose(file_);
    file_ = nullptr;
  }
}

bool gzip_ostream::is_open() const { return file_ != nullptr; }

std::string gzip_compress_string(const std::string &input, int level) {
  libdeflate_compressor *compressor = libdeflate_alloc_compressor(level);
  if (!compressor) {
    throw std::runtime_error("Failed allocating libdeflate gzip compressor.");
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
  for (; reads_processed < num_reads; reads_processed++) {
    if (!std::getline(*input_stream, id_array[reads_processed]))
      break;
    remove_CR_from_end(id_array[reads_processed]);
    if (!std::getline(*input_stream, read_array[reads_processed]))
      throw std::runtime_error(kInvalidFastqError);
    remove_CR_from_end(read_array[reads_processed]);
    if (fasta_flag)
      continue;
    std::string plus_line;
    if (!std::getline(*input_stream, plus_line))
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
  if (num_reads == 0)
    return;

  if (bgzf_flag) {
    write_bgzf_fastq_block(output_stream, id_array, read_array, quality_array,
                           num_reads, num_thr, compression_level, use_crlf,
                           fasta_mode);
  } else if (gzip_flag) {
    std::vector<std::string> compressed(static_cast<size_t>(num_thr));
    const std::vector<read_range> thread_ranges =
        compute_read_ranges(num_reads, num_thr);
#pragma omp parallel num_threads(num_thr)
    {
      const int tid = omp_get_thread_num();
      tl_plain_buffer.clear();
      const read_range &range = thread_ranges[static_cast<size_t>(tid)];
      append_fastq_records_range(tl_plain_buffer, id_array, read_array,
                                 quality_array, range.start, range.end,
                                 use_crlf, fasta_mode);
      compressed[tid] =
          gzip_compress_string(tl_plain_buffer, compression_level);
    }
    for (int i = 0; i < num_thr; i++)
      output_stream.write(compressed[i].data(), compressed[i].size());
  } else {
    for (uint32_t i = 0; i < num_reads; i++) {
      std::string rec;
      append_fastq_record(rec, id_array[i], read_array[i],
                          quality_array ? &quality_array[i] : nullptr, use_crlf,
                          fasta_mode);
      output_stream.write(rec.data(), rec.size());
    }
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

  const std::vector<read_range> thread_ranges =
      compute_read_ranges(num_reads, num_thr);
  std::vector<std::vector<std::string>> bgzf_blocks(num_thr);

#pragma omp parallel num_threads(num_thr)
  {
    const int tid = omp_get_thread_num();
    tl_plain_buffer.clear();
    const read_range &range = thread_ranges[static_cast<size_t>(tid)];
    append_fastq_records_range(tl_plain_buffer, id_array, read_array,
                               quality_array, range.start, range.end, use_crlf,
                               fasta_mode);
    bgzf_blocks[tid] = bgzf_compress_buffer(tl_plain_buffer, compression_level);
  }

  for (int i = 0; i < num_thr; i++) {
    for (const auto &block : bgzf_blocks[i]) {
      output_stream.write(block.data(), block.size());
    }
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

  uint32_t num_nalpha = static_cast<uint32_t>(non_alpha_cols.size());
  uint32_t num_alpha = static_cast<uint32_t>(alpha_cols.size());
  uint32_t count_sz = static_cast<uint32_t>(col_counts.size());

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

  std::vector<char> buffer;
  buffer.reserve(est_size);

  auto write_u32 = [&](uint32_t val) {
    char bytes[4];
    std::memcpy(bytes, &val, 4);
    buffer.insert(buffer.end(), bytes, bytes + 4);
  };

  write_u32(num_nalpha);
  write_u32(num_alpha);
  write_u32(count_sz);
  buffer.insert(buffer.end(), col_counts.begin(), col_counts.end());

  for (const auto &c : non_alpha_cols) {
    write_u32(static_cast<uint32_t>(c.size()));
    buffer.insert(buffer.end(), c.begin(), c.end());
  }
  for (size_t i = 0; i < new_alpha_cols.size(); ++i) {
    buffer.push_back(alpha_fmts[i]);
    write_u32(static_cast<uint32_t>(new_alpha_cols[i].size()));
    buffer.insert(buffer.end(), new_alpha_cols[i].begin(),
                  new_alpha_cols[i].end());
  }

  if (pack_only) {
    std::ofstream fout(output_path, std::ios::binary);
    if (!fout)
      throw std::runtime_error("Failed to open ID pack output file.");
    fout.write(reinterpret_cast<const char *>(buffer.data()), buffer.size());
    fout.close();
  } else {
    const std::string temp_id_path = std::string(output_path) + ".tmp_id_enc";
    std::ofstream out(temp_id_path, std::ios::binary);
    if (!out)
      throw std::runtime_error("Failed to open temporary ID encoding file.");
    out.write(reinterpret_cast<const char *>(buffer.data()), buffer.size());
    out.close();

    try {
      bsc::BSC_compress(temp_id_path.c_str(), output_path);
    } catch (const std::exception &e) {
      std::filesystem::remove(temp_id_path);
      throw std::runtime_error(std::string("BSC compression failed: ") +
                               e.what());
    }
    std::filesystem::remove(temp_id_path);
  }
}

void decompress_id_block(const char *input_path, std::string *id_array,
                         const uint32_t &num_ids, bool pack_only) {
  if (num_ids == 0)
    return;

  std::string temp_id_path;
  if (pack_only) {
    temp_id_path = input_path;
  } else {
    temp_id_path = std::string(input_path) + ".tmp_id_dec";
    safe_bsc_decompress(input_path, temp_id_path);
  }

  std::ifstream id_in(temp_id_path, std::ios::binary | std::ios::ate);
  if (!id_in) {
    if (!pack_only)
      std::filesystem::remove(temp_id_path);
    throw std::runtime_error("Failed to open temporary decompressed ID file.");
  }
  const size_t r_size = static_cast<size_t>(id_in.tellg());
  id_in.seekg(0, std::ios::beg);
  std::vector<uint8_t> buffer(r_size);
  if (!id_in.read(reinterpret_cast<char *>(buffer.data()), r_size)) {
    id_in.close();
    if (!pack_only)
      std::filesystem::remove(temp_id_path);
    throw std::runtime_error("Failed to read decompressed ID block.");
  }
  id_in.close();
  if (!pack_only)
    std::filesystem::remove(temp_id_path);

  const char *curr = reinterpret_cast<const char *>(buffer.data());
  const char *end = curr + r_size;
  auto read_u32 = [&]() {
    if (curr + 4 > end)
      throw std::runtime_error("Truncated ID block metadata");
    uint32_t val;
    std::memcpy(&val, curr, 4);
    curr += 4;
    return val;
  };

  uint32_t num_nalpha = read_u32();
  uint32_t num_alpha = read_u32();
  uint32_t count_sz = read_u32();
  if (count_sz != num_ids)
    throw std::runtime_error("ID block mismatch in count");

  if (curr + count_sz > end)
    throw std::runtime_error("Truncated ID counts");
  const uint8_t *counts_ptr = reinterpret_cast<const uint8_t *>(curr);
  curr += count_sz;

  std::vector<const char *> nalpha_ptrs(num_nalpha);
  for (uint32_t i = 0; i < num_nalpha; ++i) {
    uint32_t len = read_u32();
    if (curr + len > end)
      throw std::runtime_error("Truncated ID non-alpha column");
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
      throw std::runtime_error("Truncated ID alpha column fmt");
    alpha_fmts[i] = *curr++;
    uint32_t len = read_u32();
    if (curr + len > end)
      throw std::runtime_error("Truncated ID alpha column");
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
  for (uint32_t i = 0; i < num_lines; i++) {
    for (char &c : quality_array[i]) {
      c = quantization_table[static_cast<uint8_t>(c)];
    }
  }
}

void quantize_quality_qvz(std::string *quality_array, const uint32_t &num_lines,
                          uint32_t *str_len_array, double qv_ratio) {
  qvz::qv_options_t opts;
  opts.verbose = 0;
  opts.stats = 0;
  opts.clusters = 1;
  opts.uncompressed = 0;
  opts.ratio = qv_ratio;
  opts.distortion = DISTORTION_MSE;
  opts.mode = MODE_FIXED;
  size_t max_readlen = 0;
  for (uint32_t i = 0; i < num_lines; i++) {
    if (str_len_array[i] > max_readlen)
      max_readlen = str_len_array[i];
  }
  qvz::encode(&opts, static_cast<uint32_t>(max_readlen), num_lines,
              quality_array, str_len_array);
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
  try {
    bsc::BSC_str_array_decompress(input_path.c_str(), string_array, num_strings,
                                  string_lengths);
  } catch (const std::exception &e) {
    throw std::runtime_error("BSC string array decompression failed for " +
                             input_path + ": " + e.what());
  }
}

void generate_illumina_binning_table(char *illumina_binning_table) {
  for (int i = 0; i < 128; i++) {
    if (i <= 33 + 1)
      illumina_binning_table[i] = (char)i;
    else if (i <= 33 + 9)
      illumina_binning_table[i] = (char)(33 + 6);
    else if (i <= 33 + 19)
      illumina_binning_table[i] = (char)(33 + 15);
    else if (i <= 33 + 24)
      illumina_binning_table[i] = (char)(33 + 22);
    else if (i <= 33 + 29)
      illumina_binning_table[i] = (char)(33 + 27);
    else if (i <= 33 + 34)
      illumina_binning_table[i] = (char)(33 + 33);
    else if (i <= 33 + 39)
      illumina_binning_table[i] = (char)(33 + 37);
    else
      illumina_binning_table[i] = (char)(33 + 40);
  }
}

void generate_binary_binning_table(char *binary_binning_table,
                                   const unsigned int thr,
                                   const unsigned int high,
                                   const unsigned int low) {
  unsigned int split = 33 + thr;
  for (unsigned int i = 0; i < split; i++)
    binary_binning_table[i] = 33 + low;
  for (unsigned int i = split; i <= 127; i++)
    binary_binning_table[i] = 33 + high;
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
  if (!fin)
    return;

  fin.seekg(0, std::ios::end);
  compressed_size = fin.tellg();
  fin.seekg(0, std::ios::beg);

  while (true) {
    unsigned char header[10];
    if (!fin.read(reinterpret_cast<char *>(header), 10))
      break;
    if (header[0] != 0x1f || header[1] != 0x8b) {
      if (member_count == 0)
        return;
      break;
    }
    is_gzipped = true;
    member_count++;
    const uint8_t current_flg = header[3];
    uint16_t current_bsiz = 0;

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
          if (member_count == 1) {
            is_bgzf = true;
            bgzf_block_size = current_bsiz + 1;
          }
        } else
          fin.seekg(slen, std::ios::cur);
      }
    }
    if (current_flg & 0x08) {
      char c;
      while (fin.read(&c, 1) && c != '\0') {
        if (member_count == 1)
          name += c;
      }
    }
    if (current_flg & 0x10) {
      char c;
      while (fin.read(&c, 1) && c != '\0')
        ;
    }
    if (current_flg & 0x02)
      fin.seekg(2, std::ios::cur);

    fin.seekg(-8, std::ios::end); // Fallback for single-member
    uint32_t isize;
    fin.read(reinterpret_cast<char *>(&isize), 4);
    uncompressed_size = isize; // Simpler fallback for now
    break;
  }
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
    encoded_value |= ((uint64_t)(byte & 0x7f) << shift);
    shift += 7;
  } while (byte & 0x80);
  return zigzag_decode64(encoded_value);
}

} // namespace spring
