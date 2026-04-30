// Normalizes input FASTQ/FASTA records into Spring's temporary block files and
// side streams before reordering, encoding, and archive assembly.

#include "preprocess.h"
#include "progress.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <omp.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "core_utils.h"

#include "io_utils.h"
#include "libbsc/bsc.h"
#include "params.h"
#include "parse_utils.h"

namespace spring {

namespace {

struct preprocess_paths {
  std::array<std::string, 2> input_paths;
  std::array<std::string, 2> clean_read_paths;
  std::array<std::string, 2> n_read_paths;
  std::array<std::string, 2> n_read_order_paths;
  std::array<std::string, 2> id_output_paths;
  std::array<std::string, 2> quality_output_paths;
  std::array<std::string, 2> read_block_paths;
  std::array<std::string, 2> read_length_paths;
};

struct short_read_thread_buffers {
  std::string clean_read_bytes;
  std::string n_read_bytes;
  std::vector<uint32_t> n_read_positions;
  std::string tail_info_bytes;    // Per-clean-read uint16_t tail encodings
  std::string atac_adapter_bytes; // Per-clean-read ATAC adapter encodings
  uint32_t clean_read_count = 0;
};

std::string block_file_path(const std::string &base_path,
                            const uint32_t block_num) {
  return base_path + "." + std::to_string(block_num);
}

std::string compressed_block_file_path(const std::string &base_path,
                                       const uint32_t block_num) {
  return block_file_path(base_path, block_num) + ".bsc";
}

uint32_t block_count(const uint64_t num_reads,
                     const uint32_t num_reads_per_block) {
  if (num_reads == 0)
    return 0;
  return 1 + (num_reads - 1) / num_reads_per_block;
}

void append_uint16(std::string &buffer, const uint16_t value) {
  buffer.append(reinterpret_cast<const char *>(&value), sizeof(uint16_t));
}

// Strip a terminal poly-A or poly-T run longer than POLY_AT_TAIL_MIN_LEN.
// Returns a uint16_t encoding: bit 0 = base (0=A,1=T), bits 15:1 = run length.
// Returns 0 if nothing was stripped. Modifies read_str and read_length in
// place.
uint16_t detect_and_strip_tail(std::string &read_str, uint32_t &read_length) {
  if (read_length <= POLY_AT_TAIL_MIN_LEN)
    return 0;
  const char last = read_str[read_length - 1];
  if (last != 'A' && last != 'T')
    return 0;
  uint32_t run = 0;
  while (run < read_length && read_str[read_length - 1 - run] == last)
    ++run;
  if (run <= POLY_AT_TAIL_MIN_LEN)
    return 0;
  read_length -= run;
  read_str.resize(read_length);
  return static_cast<uint16_t>((run << 1) | (last == 'T' ? 1 : 0));
}

// Strip terminal Tn5/Nextera adapter read-through from the 3' end of a read.
// Returns a uint8_t encoding: bit 0 = adapter id (0=forward, 1=reverse
// complement), bits 7:1 = overlap length. Returns 0 if nothing was stripped.
uint8_t detect_atac_adapter_tail_info(const std::string &read_str,
                                      const uint32_t read_length) {
  if (read_length < ATAC_ADAPTER_MIN_MATCH)
    return 0;

  static constexpr std::array<std::string_view, 2> kAdapters = {
      "CTGTCTCTTATACACATCT", "AGATGTGTATAAGAGACAG"};

  for (uint16_t adapter_id = 0; adapter_id < kAdapters.size(); ++adapter_id) {
    const std::string_view adapter = kAdapters[adapter_id];
    const uint32_t max_overlap =
        std::min<uint32_t>(read_length, static_cast<uint32_t>(adapter.size()));
    for (uint32_t overlap = max_overlap; overlap >= ATAC_ADAPTER_MIN_MATCH;
         --overlap) {
      const size_t read_offset = static_cast<size_t>(read_length - overlap);
      if (read_str.compare(read_offset, overlap,
                           adapter.substr(0, overlap).data(), overlap) == 0) {
        return static_cast<uint8_t>((overlap << 1) | adapter_id);
      }
      if (overlap == ATAC_ADAPTER_MIN_MATCH)
        break;
    }
  }

  return 0;
}

uint8_t detect_and_strip_atac_adapter_tail(std::string &read_str,
                                           uint32_t &read_length) {
  const uint8_t strip_info =
      detect_atac_adapter_tail_info(read_str, read_length);
  const uint32_t overlap = strip_info >> 1;
  if (overlap > 0) {
    read_length -= overlap;
    read_str.resize(read_length);
  }
  return strip_info;
}

bool is_grouped_index_archive_note(const std::string &note) {
  return note.find("index-group") != std::string::npos;
}

bool is_grouped_read3_archive_note(const std::string &note) {
  return note.find("read3-group") != std::string::npos;
}

bool strip_grouped_index_suffix_from_id(std::string &id,
                                        const std::string &stream_1_seq,
                                        const bool paired_index) {
  const size_t last_colon = id.rfind(':');
  if (last_colon == std::string::npos || last_colon + 1 >= id.size()) {
    return false;
  }

  const std::string_view suffix(id.data() + last_colon + 1,
                                id.size() - last_colon - 1);
  if (paired_index) {
    const size_t plus = suffix.find('+');
    if (plus == std::string::npos || plus == 0) {
      return false;
    }
    if (suffix.substr(0, plus) != stream_1_seq) {
      return false;
    }
  } else if (suffix != stream_1_seq) {
    return false;
  }

  id.resize(last_colon + 1);
  return true;
}

void append_encoded_dna_bits(std::string &buffer, const std::string &read) {
  static const std::array<uint8_t, 128> dna_to_int = []() {
    std::array<uint8_t, 128> table{};
    table[static_cast<uint8_t>('A')] = 0;
    table[static_cast<uint8_t>('C')] = 2;
    table[static_cast<uint8_t>('G')] = 1;
    table[static_cast<uint8_t>('T')] = 3;
    return table;
  }();

  const uint16_t readlen = static_cast<uint16_t>(read.size());
  append_uint16(buffer, readlen);
  const uint32_t encoded_bytes = (static_cast<uint32_t>(readlen) + 3U) / 4U;
  const size_t old_size = buffer.size();
  buffer.resize(old_size + encoded_bytes);
  uint8_t *out = reinterpret_cast<uint8_t *>(&buffer[old_size]);

  for (uint32_t byte_index = 0; byte_index < encoded_bytes; ++byte_index) {
    uint8_t packed = 0;
    for (uint32_t base_index = 0; base_index < 4; ++base_index) {
      const uint32_t read_index = byte_index * 4U + base_index;
      if (read_index >= readlen)
        break;
      const uint8_t encoded =
          dna_to_int[static_cast<uint8_t>(read[read_index])] & 0x03;
      packed |= static_cast<uint8_t>(encoded << (2U * base_index));
    }
    out[byte_index] = packed;
  }
}

void append_encoded_dna_n_bits(std::string &buffer, const std::string &read) {
  static const std::array<uint8_t, 128> dna_n_to_int = []() {
    std::array<uint8_t, 128> table{};
    table[static_cast<uint8_t>('A')] = 0;
    table[static_cast<uint8_t>('C')] = 2;
    table[static_cast<uint8_t>('G')] = 1;
    table[static_cast<uint8_t>('T')] = 3;
    table[static_cast<uint8_t>('N')] = 4;
    return table;
  }();

  const uint16_t readlen = static_cast<uint16_t>(read.size());
  append_uint16(buffer, readlen);
  const uint32_t encoded_bytes = (static_cast<uint32_t>(readlen) + 1U) / 2U;
  const size_t old_size = buffer.size();
  buffer.resize(old_size + encoded_bytes);
  uint8_t *out = reinterpret_cast<uint8_t *>(&buffer[old_size]);

  for (uint32_t byte_index = 0; byte_index < encoded_bytes; ++byte_index) {
    uint8_t packed = 0;
    for (uint32_t base_index = 0; base_index < 2; ++base_index) {
      const uint32_t read_index = byte_index * 2U + base_index;
      if (read_index >= readlen)
        break;
      const uint8_t encoded =
          dna_n_to_int[static_cast<uint8_t>(read[read_index])] & 0x0F;
      packed |= static_cast<uint8_t>(encoded << (4U * base_index));
    }
    out[byte_index] = packed;
  }
}

uint32_t flush_short_read_thread_buffers(
    const std::vector<short_read_thread_buffers> &thread_buffers,
    std::ofstream &clean_output, std::ofstream &n_read_output,
    std::ofstream &n_read_order_output, std::ofstream *tail_output,
    std::ofstream *atac_adapter_output) {
  uint32_t clean_read_count = 0;
  for (const short_read_thread_buffers &thread_buffer : thread_buffers) {
    if (!thread_buffer.clean_read_bytes.empty()) {
      clean_output.write(
          thread_buffer.clean_read_bytes.data(),
          static_cast<std::streamsize>(thread_buffer.clean_read_bytes.size()));
    }
    if (!thread_buffer.n_read_bytes.empty()) {
      n_read_output.write(
          thread_buffer.n_read_bytes.data(),
          static_cast<std::streamsize>(thread_buffer.n_read_bytes.size()));
    }
    if (!thread_buffer.n_read_positions.empty()) {
      n_read_order_output.write(
          byte_ptr(thread_buffer.n_read_positions.data()),
          static_cast<std::streamsize>(thread_buffer.n_read_positions.size() *
                                       sizeof(uint32_t)));
    }
    if (tail_output && !thread_buffer.tail_info_bytes.empty()) {
      tail_output->write(
          thread_buffer.tail_info_bytes.data(),
          static_cast<std::streamsize>(thread_buffer.tail_info_bytes.size()));
    }
    if (atac_adapter_output && !thread_buffer.atac_adapter_bytes.empty()) {
      atac_adapter_output->write(thread_buffer.atac_adapter_bytes.data(),
                                 static_cast<std::streamsize>(
                                     thread_buffer.atac_adapter_bytes.size()));
    }
    clean_read_count += thread_buffer.clean_read_count;
  }

  return clean_read_count;
}

void write_raw_string_block(const std::string &output_path,
                            std::string *strings, const uint32_t string_count,
                            const uint32_t *string_lengths) {
  std::ofstream output(output_path, std::ios::binary);
  if (!output.is_open()) {
    throw std::runtime_error("Failed to open raw string block output: " +
                             output_path);
  }
  for (uint32_t i = 0; i < string_count; i++) {
    output.write(strings[i].data(), string_lengths[i]);
  }
}

void open_input_stream(std::ifstream &file_stream, std::istream *&input_stream,
                       std::unique_ptr<gzip_istream> &gzip_stream,
                       const std::string &path, const bool gzip_enabled) {
  if (gzip_enabled) {
    gzip_stream = std::make_unique<gzip_istream>(path);
    input_stream = gzip_stream.get();
    return;
  }

  file_stream.open(path, std::ios::binary);
  input_stream = &file_stream;
  gzip_stream.reset();
}

void reset_input_stream(std::ifstream &file_stream, std::istream *&input_stream,
                        std::unique_ptr<gzip_istream> &gzip_stream,
                        const std::string &path, const bool gzip_enabled) {
  if (gzip_enabled) {
    file_stream.close();
    gzip_stream.reset();
    input_stream = nullptr;
    open_input_stream(file_stream, input_stream, gzip_stream, path, true);
    return;
  }

  file_stream.clear();
  file_stream.seekg(0);
}

void close_input_stream(std::ifstream &file_stream, std::istream *&input_stream,
                        std::unique_ptr<gzip_istream> &gzip_stream,
                        const bool gzip_enabled) {
  if (gzip_enabled) {
    gzip_stream.reset();
    input_stream = nullptr;
  }
  file_stream.close();
}

preprocess_paths build_preprocess_paths(const std::string &input_path_1,
                                        const std::string &input_path_2,
                                        const std::string &temp_dir) {
  preprocess_paths paths;
  paths.input_paths = {input_path_1, input_path_2};
  paths.clean_read_paths = {temp_dir + "/input_clean_1.dna",
                            temp_dir + "/input_clean_2.dna"};
  paths.n_read_paths = {temp_dir + "/input_N.dna", temp_dir + "/input_N.dna.2"};
  paths.n_read_order_paths = {temp_dir + "/read_order_N.bin",
                              temp_dir + "/read_order_N.bin.2"};
  paths.id_output_paths = {temp_dir + "/id_1", temp_dir + "/id_2"};
  paths.quality_output_paths = {temp_dir + "/quality_1",
                                temp_dir + "/quality_2"};
  paths.read_block_paths = {temp_dir + "/read_1", temp_dir + "/read_2"};
  paths.read_length_paths = {temp_dir + "/readlength_1",
                             temp_dir + "/readlength_2"};
  return paths;
}

uint32_t reads_for_thread_step(const uint32_t reads_in_step,
                               const uint32_t num_reads_per_block,
                               const uint64_t thread_id) {
  return std::min((uint64_t)reads_in_step,
                  (thread_id + 1) * num_reads_per_block) -
         thread_id * num_reads_per_block;
}

void open_preprocess_streams(
    std::array<std::ifstream, 2> &input_files,
    std::array<std::ofstream, 2> &clean_outputs,
    std::array<std::ofstream, 2> &n_read_outputs,
    std::array<std::ofstream, 2> &n_read_order_outputs,
    std::array<std::ofstream, 2> &id_outputs,
    std::array<std::ofstream, 2> &quality_outputs,
    std::array<std::istream *, 2> &input_streams,
    std::array<std::unique_ptr<gzip_istream>, 2> &gzip_streams,
    const preprocess_paths &paths, const compression_params &compression_params,
    const bool gzip_enabled) {
  for (int stream_index = 0; stream_index < 2; stream_index++) {
    if (stream_index == 1 && !compression_params.encoding.paired_end)
      continue;

    open_input_stream(input_files[stream_index], input_streams[stream_index],
                      gzip_streams[stream_index],
                      paths.input_paths[stream_index], gzip_enabled);
    if (compression_params.encoding.long_flag)
      continue;

    clean_outputs[stream_index].open(paths.clean_read_paths[stream_index],
                                     std::ios::binary);
    n_read_outputs[stream_index].open(paths.n_read_paths[stream_index],
                                      std::ios::binary);
    n_read_order_outputs[stream_index].open(
        paths.n_read_order_paths[stream_index], std::ios::binary);
    if (!compression_params.encoding.preserve_order) {
      if (compression_params.encoding.preserve_id)
        id_outputs[stream_index].open(paths.id_output_paths[stream_index],
                                      std::ios::binary);
      if (compression_params.encoding.preserve_quality)
        quality_outputs[stream_index].open(
            paths.quality_output_paths[stream_index], std::ios::binary);
    }
  }
}

void close_preprocess_streams(
    std::array<std::ifstream, 2> &input_files,
    std::array<std::ofstream, 2> &clean_outputs,
    std::array<std::ofstream, 2> &n_read_outputs,
    std::array<std::ofstream, 2> &n_read_order_outputs,
    std::array<std::ofstream, 2> &id_outputs,
    std::array<std::ofstream, 2> &quality_outputs,
    std::array<std::istream *, 2> &input_streams,
    std::array<std::unique_ptr<gzip_istream>, 2> &gzip_streams,
    const compression_params &compression_params, const bool gzip_enabled) {
  for (int stream_index = 0; stream_index < 2; stream_index++) {
    if (stream_index == 1 && !compression_params.encoding.paired_end)
      continue;

    close_input_stream(input_files[stream_index], input_streams[stream_index],
                       gzip_streams[stream_index], gzip_enabled);
    if (compression_params.encoding.long_flag)
      continue;

    clean_outputs[stream_index].close();
    n_read_outputs[stream_index].close();
    n_read_order_outputs[stream_index].close();
    if (!compression_params.encoding.preserve_order) {
      if (compression_params.encoding.preserve_id)
        id_outputs[stream_index].close();
      if (compression_params.encoding.preserve_quality)
        quality_outputs[stream_index].close();
    }
  }
}

// Detect whether the FASTQ uses "+ID" format or just "+" for quality headers.
// Checks the first FASTQ record and returns true if the plus line contains
// anything beyond the initial '+' character.
void detect_quality_header_format(
    std::array<std::ifstream, 2> &input_files,
    std::array<std::istream *, 2> &input_streams,
    std::array<std::unique_ptr<gzip_istream>, 2> &gzip_streams,
    const preprocess_paths &paths, const compression_params &compression_params,
    const bool gzip_enabled, bool &quality_header_has_id) {
  if (compression_params.encoding.fasta_mode) {
    quality_header_has_id = false;
    return;
  }

  // Read first record from first stream to detect format
  std::string line;
  // Line 1: Header
  if (!std::getline(*input_streams[0], line))
    return;
  // Line 2: Sequence
  if (!std::getline(*input_streams[0], line))
    return;
  // Line 3: Plus line
  if (!std::getline(*input_streams[0], line))
    return;

  // Check if plus line has content beyond the '+' character
  // Remove any trailing CR if present (std::getline removes \n already)
  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }

  quality_header_has_id = line.length() > 1;

  SPRING_LOG_DEBUG("Quality header detection: plus_line_length=" +
                   std::to_string(line.length()) + ", has_id=" +
                   std::string(quality_header_has_id ? "true" : "false"));

  // Reset stream to beginning for actual processing
  reset_input_stream(input_files[0], input_streams[0], gzip_streams[0],
                     paths.input_paths[0], gzip_enabled);
  if (compression_params.encoding.paired_end) {
    reset_input_stream(input_files[1], input_streams[1], gzip_streams[1],
                       paths.input_paths[1], gzip_enabled);
  }
}

void detect_paired_id_pattern(
    std::array<std::ifstream, 2> &input_files,
    std::array<std::istream *, 2> &input_streams,
    std::array<std::unique_ptr<gzip_istream>, 2> &gzip_streams,
    const preprocess_paths &paths, const compression_params &compression_params,
    const bool gzip_enabled, uint8_t &paired_id_code, bool &paired_id_match) {
  if (!compression_params.encoding.paired_end ||
      !compression_params.encoding.preserve_id)
    return;

  std::string id_1;
  std::string id_2;
  std::getline(*input_streams[0], id_1);
  std::getline(*input_streams[1], id_2);
  paired_id_code = find_id_pattern(id_1, id_2);
  paired_id_match = paired_id_code != 0;

  for (int stream_index = 0; stream_index < 2; stream_index++) {
    reset_input_stream(input_files[stream_index], input_streams[stream_index],
                       gzip_streams[stream_index],
                       paths.input_paths[stream_index], gzip_enabled);
  }
}

void merge_paired_n_reads(const preprocess_paths &paths,
                          const std::array<uint64_t, 2> &num_reads,
                          const std::array<uint64_t, 2> &num_reads_clean) {
  std::ofstream merged_n_read_output(paths.n_read_paths[0],
                                     std::ios::app | std::ios::binary);
  std::ifstream mate_n_read_input(paths.n_read_paths[1], std::ios::binary);
  merged_n_read_output << mate_n_read_input.rdbuf();
  remove(paths.n_read_paths[1].c_str());

  std::ofstream merged_n_read_order_output(paths.n_read_order_paths[0],
                                           std::ios::app | std::ios::binary);
  std::ifstream mate_n_read_order_input(paths.n_read_order_paths[1],
                                        std::ios::binary);
  const uint32_t mate_n_read_count = num_reads[1] - num_reads_clean[1];
  if (mate_n_read_count > 0) {
    std::vector<uint32_t> n_read_orders(mate_n_read_count);
    mate_n_read_order_input.read(byte_ptr(n_read_orders.data()),
                                 mate_n_read_count * sizeof(uint32_t));
    for (uint32_t &n_read_order : n_read_orders) {
      n_read_order += num_reads[0];
    }
    merged_n_read_order_output.write(byte_ptr(n_read_orders.data()),
                                     mate_n_read_count * sizeof(uint32_t));
  }
  remove(paths.n_read_order_paths[1].c_str());
}

void remove_redundant_mate_ids(const preprocess_paths &paths,
                               const compression_params &compression_params,
                               const bool paired_id_match,
                               const std::array<uint64_t, 2> &num_reads,
                               const uint32_t num_reads_per_block) {
  if (!compression_params.encoding.paired_end || !paired_id_match)
    return;

  if (!compression_params.encoding.long_flag &&
      !compression_params.encoding.preserve_order) {
    remove(paths.id_output_paths[1].c_str());
    return;
  }

  const uint32_t num_blocks = block_count(num_reads[0], num_reads_per_block);
  for (uint32_t block_index = 0; block_index < num_blocks; block_index++) {
    const std::string block_path =
        compression_params.encoding.long_flag
            ? compressed_block_file_path(paths.id_output_paths[1], block_index)
            : block_file_path(paths.id_output_paths[1], block_index);
    remove(block_path.c_str());
  }
}

uint32_t max_read_length_in_step(const std::vector<uint32_t> &read_lengths,
                                 const uint32_t reads_in_step) {
  if (reads_in_step == 0)
    return 0;
  return *(std::max_element(read_lengths.begin(),
                            read_lengths.begin() + reads_in_step));
}

} // namespace

bool has_non_acgtn_symbol(const std::string &read) {
  return std::ranges::any_of(read, [](char base) {
    if (base == '\r' || base == '\n' || base == ' ')
      return false;
    return base != 'A' && base != 'C' && base != 'G' && base != 'T' &&
           base != 'N';
  });
}

uint32_t detect_max_read_length_in_file(const std::string &path,
                                        bool fasta_input, bool &use_crlf,
                                        bool &contains_non_acgtn_symbols) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open())
    throw std::runtime_error("Can't open file for pre-scan: " + path);

  uint32_t max_len = 0;
  std::string line;
  if (fasta_input) {
    uint32_t current_len = 0;
    while (std::getline(input, line)) {
      if (!line.empty() && line.back() == '\r')
        use_crlf = true;
      if (line.empty())
        continue;
      if (line[0] == '>') {
        max_len = std::max(max_len, current_len);
        current_len = 0;
      } else {
        if (!contains_non_acgtn_symbols && has_non_acgtn_symbol(line)) {
          contains_non_acgtn_symbols = true;
        }
        current_len += line.length();
      }
    }
    max_len = std::max(max_len, current_len);
  } else {
    // FASTQ: Seq is 2nd line of every 4.
    while (std::getline(input, line)) { // 1: Header
      if (!line.empty() && line.back() == '\r')
        use_crlf = true;
      if (std::getline(input, line)) { // 2: Sequence
        if (!contains_non_acgtn_symbols && has_non_acgtn_symbol(line)) {
          contains_non_acgtn_symbols = true;
        }
        max_len = std::max(max_len, (uint32_t)line.length());
      }
      std::getline(input, line); // 3: +
      std::getline(input, line); // 4: Quality
    }
  }
  return max_len;
}

uint32_t detect_max_read_length(const std::string &infile_1,
                                const std::string &infile_2,
                                const bool paired_end, const bool fasta_input,
                                bool &use_crlf,
                                bool &contains_non_acgtn_symbols) {
  SPRING_LOG_INFO("Auto-detecting read lengths and line endings ...");
  use_crlf = false;
  contains_non_acgtn_symbols = false;
  uint32_t max_len_1 = detect_max_read_length_in_file(
      infile_1, fasta_input, use_crlf, contains_non_acgtn_symbols);
  uint32_t max_len_2 = 0;
  if (paired_end) {
    max_len_2 = detect_max_read_length_in_file(infile_2, fasta_input, use_crlf,
                                               contains_non_acgtn_symbols);
  }
  return std::max(max_len_1, max_len_2);
}

void preprocess(const std::string &infile_1, const std::string &infile_2,
                const std::string &temp_dir, compression_params &cp,
                const bool &fasta_input, ProgressBar *progress) {
  SPRING_LOG_DEBUG(
      "Preprocess start: temp_dir=" + temp_dir + ", input1=" + infile_1 +
      (cp.encoding.paired_end ? (", input2=" + infile_2) : std::string()) +
      ", long_mode=" + std::string(cp.encoding.long_flag ? "true" : "false") +
      ", paired_end=" + std::string(cp.encoding.paired_end ? "true" : "false") +
      ", preserve_order=" +
      std::string(cp.encoding.preserve_order ? "true" : "false") +
      ", preserve_id=" +
      std::string(cp.encoding.preserve_id ? "true" : "false") +
      ", preserve_quality=" +
      std::string(cp.encoding.preserve_quality ? "true" : "false") +
      ", fasta_input=" + std::string(fasta_input ? "true" : "false"));
  const preprocess_paths paths =
      build_preprocess_paths(infile_1, infile_2, temp_dir);
  std::array<std::ifstream, 2> input_files;
  std::array<std::ofstream, 2> clean_outputs;
  std::array<std::ofstream, 2> n_read_outputs;
  std::array<std::ofstream, 2> n_read_order_outputs;
  std::array<std::ofstream, 2> id_outputs;
  std::array<std::ofstream, 2> quality_outputs;
  std::array<std::istream *, 2> input_streams = {nullptr, nullptr};
  std::array<std::unique_ptr<gzip_istream>, 2> gzip_streams{};

  open_preprocess_streams(input_files, clean_outputs, n_read_outputs,
                          n_read_order_outputs, id_outputs, quality_outputs,
                          input_streams, gzip_streams, paths, cp, false);

  // Determine whether poly-A/T tail stripping should be applied.
  // Only active for: short-read, non-preserve_order, rna assay with
  // high-confidence auto-detection OR explicit -y rna (confidence == "N/A").
  const bool apply_poly_at =
      !cp.encoding.long_flag && !cp.encoding.preserve_order &&
      (cp.read_info.assay == "rna" || cp.read_info.assay == "sc-rna") &&
      (cp.read_info.assay_confidence == "N/A" ||
       cp.read_info.assay_confidence.starts_with("high"));

  const bool allow_grouped_atac_adapter_strip =
      !is_grouped_index_archive_note(cp.read_info.note) &&
      !is_grouped_read3_archive_note(cp.read_info.note);
  const bool maybe_apply_atac_adapter_strip =
      !fasta_input && !cp.encoding.long_flag &&
      (cp.read_info.assay == "atac" || cp.read_info.assay == "sc-atac") &&
      (cp.read_info.assay_confidence == "N/A" ||
       cp.read_info.assay_confidence.starts_with("high") ||
       cp.read_info.assay_confidence.starts_with("medium")) &&
      allow_grouped_atac_adapter_strip;
  constexpr double kMinAtacAdapterBasesPerRead = 1.0;
  bool atac_adapter_strip_active = false;
  bool atac_adapter_strip_decided = !maybe_apply_atac_adapter_strip;

  std::array<std::ofstream, 2> tail_outputs;
  if (apply_poly_at) {
    tail_outputs[0].open(temp_dir + "/tail_1.bin", std::ios::binary);
    if (!tail_outputs[0])
      throw std::runtime_error("Failed to open tail_1.bin for writing");
    if (cp.encoding.paired_end) {
      tail_outputs[1].open(temp_dir + "/tail_2.bin", std::ios::binary);
      if (!tail_outputs[1])
        throw std::runtime_error("Failed to open tail_2.bin for writing");
    }
    SPRING_LOG_DEBUG("poly-A/T tail stripping enabled (min run length > " +
                     std::to_string(POLY_AT_TAIL_MIN_LEN) + " bp)");
  }

  std::array<std::ofstream, 2> atac_adapter_outputs;
  if (maybe_apply_atac_adapter_strip) {
    atac_adapter_outputs[0].open(temp_dir + "/atac_adapter_1.bin",
                                 std::ios::binary);
    if (!atac_adapter_outputs[0])
      throw std::runtime_error("Failed to open atac_adapter_1.bin for writing");
    if (cp.encoding.paired_end) {
      atac_adapter_outputs[1].open(temp_dir + "/atac_adapter_2.bin",
                                   std::ios::binary);
      if (!atac_adapter_outputs[1])
        throw std::runtime_error(
            "Failed to open atac_adapter_2.bin for writing");
    }
    SPRING_LOG_DEBUG("ATAC adapter stripping enabled (min overlap >= " +
                     std::to_string(ATAC_ADAPTER_MIN_MATCH) + " bp)");
  }

  // Determine whether single-cell barcode prefix stripping should be applied.
  // Only active for short-read, preserve_order archives when the assay keeps
  // the CB in R1 instead of a separate index lane.
  //
  // For sc-RNA, CB stripping is always enabled when no external index is
  // present, as RNA protocols consistently place barcodes in R1.
  //
  // For sc-bisulfite, we disable CB stripping entirely as bisulfite protocols
  // vary widely in their read structure - some use R1 for barcodes (like 10x),
  // others use R1 for genomic sequence (like scSPLAT). Without a reliable
  // heuristic to detect which structure is present, we conservatively skip CB
  // extraction to avoid data corruption. Future work could add
  // protocol-specific detection.
  const bool apply_cb_strip =
      !cp.encoding.long_flag && cp.encoding.preserve_order &&
      !cp.encoding.cb_prefix_source_external && cp.encoding.cb_len > 0 &&
      cp.read_info.assay == "sc-rna" &&
      !is_grouped_index_archive_note(cp.read_info.note);
  const bool maybe_strip_grouped_index_id_suffix =
      !cp.encoding.long_flag && cp.encoding.preserve_order &&
      cp.encoding.preserve_id && cp.read_info.assay == "sc-rna" &&
      is_grouped_index_archive_note(cp.read_info.note);
  bool grouped_index_id_suffix_strip_active = false;
  bool grouped_index_id_suffix_strip_decided =
      !maybe_strip_grouped_index_id_suffix;

  std::ofstream cb_seq_output, cb_qual_output;
  if (apply_cb_strip) {
    cb_seq_output.open(temp_dir + "/cb_prefix.dna", std::ios::binary);
    if (!cb_seq_output)
      throw std::runtime_error("Failed to open cb_prefix.dna for writing");
    if (cp.encoding.preserve_quality) {
      cb_qual_output.open(temp_dir + "/cb_prefix.qual", std::ios::binary);
      if (!cb_qual_output)
        throw std::runtime_error("Failed to open cb_prefix.qual for writing");
    }
    SPRING_LOG_DEBUG(
        "Single-cell R1 prefix stripping enabled (assay=" + cp.read_info.assay +
        ", cb_len=" + std::to_string(cp.encoding.cb_len) + " bp)");
  }

  uint64_t total_input_bytes = 0;
  total_input_bytes += std::filesystem::exists(infile_1)
                           ? std::filesystem::file_size(infile_1)
                           : 0;
  if (cp.encoding.paired_end) {
    total_input_bytes += std::filesystem::exists(infile_2)
                             ? std::filesystem::file_size(infile_2)
                             : 0;
  }

  uint32_t max_readlen = 0;
  std::array<uint64_t, 2> num_reads = {0, 0};
  std::array<uint64_t, 2> num_reads_clean = {0, 0};
  uint32_t num_reads_per_block;
  if (!cp.encoding.long_flag)
    num_reads_per_block = cp.encoding.num_reads_per_block;
  else
    num_reads_per_block = cp.encoding.num_reads_per_block_long;
  uint8_t paired_id_code = 0;
  bool paired_id_match = false;

  std::array<char, 128> quality_binning_table{};
  if (cp.quality.ill_bin_flag)
    generate_illumina_binning_table(quality_binning_table.data());
  if (cp.quality.bin_thr_flag)
    generate_binary_binning_table(
        quality_binning_table.data(), cp.quality.bin_thr_thr,
        cp.quality.bin_thr_high, cp.quality.bin_thr_low);

  if (!input_files[0].is_open())
    throw std::runtime_error("Error opening input file");
  if (cp.encoding.paired_end) {
    if (!input_files[1].is_open())
      throw std::runtime_error("Error opening input file");
  }
  detect_paired_id_pattern(input_files, input_streams, gzip_streams, paths, cp,
                           false, paired_id_code, paired_id_match);
  if (cp.encoding.paired_end && cp.encoding.preserve_id) {
    SPRING_LOG_DEBUG(
        "Paired ID pattern detection: code=" +
        std::to_string(static_cast<int>(paired_id_code)) +
        ", match=" + std::string(paired_id_match ? "true" : "false"));
  }

  // Detect quality header format ("+ID" vs just "+")
  bool quality_header_has_id = false;
  detect_quality_header_format(input_files, input_streams, gzip_streams, paths,
                               cp, false, quality_header_has_id);
  SPRING_LOG_DEBUG("Quality header format: " +
                   std::string(quality_header_has_id ? "+ID" : "+"));

  // Initialize integrity digests
  for (int i = 0; i < 2; ++i) {
    cp.read_info.sequence_crc[i] = 0;
    cp.read_info.quality_crc[i] = 0;
    cp.read_info.id_crc[i] = 0;
  }
  if (cp.encoding.num_thr <= 0)
    throw std::runtime_error("Number of threads must be positive.");

  uint64_t num_reads_per_step =
      (uint64_t)cp.encoding.num_thr * num_reads_per_block;
  std::vector<std::string> read_array(num_reads_per_step);
  std::vector<std::string> id_array_1(num_reads_per_step);
  std::vector<std::string> id_array_2;
  if (cp.encoding.paired_end)
    id_array_2.resize(num_reads_per_step);

  std::vector<std::string> quality_array;
  if (!fasta_input)
    quality_array.resize(num_reads_per_step);

  std::vector<uint8_t> read_contains_N_array(num_reads_per_step, 0);
  std::vector<uint32_t> read_lengths_array(num_reads_per_step);
  std::vector<uint8_t> paired_id_match_array(
      static_cast<size_t>(cp.encoding.num_thr), 0);

  std::vector<short_read_thread_buffers> short_read_buffers;
  std::string quality_chunk;
  std::string id_chunk;
  if (!cp.encoding.long_flag) {
    short_read_buffers.resize(static_cast<size_t>(cp.encoding.num_thr));
  }

  omp_set_num_threads(cp.encoding.num_thr);

  uint32_t num_blocks_done = 0;

  while (true) {
    std::array<bool, 2> done = {true, true};
    for (int stream_index = 0; stream_index < 2; stream_index++) {
      if (stream_index == 1 && !cp.encoding.paired_end)
        continue;
      done[stream_index] = false;
      std::string *id_array =
          (stream_index == 0) ? id_array_1.data() : id_array_2.data();
      uint8_t *contains_n_output =
          cp.encoding.long_flag ? nullptr : read_contains_N_array.data();
      uint32_t *quality_crc_output =
          cp.encoding.preserve_quality ? &cp.read_info.quality_crc[stream_index]
                                       : nullptr;
      uint32_t *id_crc_output = cp.encoding.preserve_id
                                    ? &cp.read_info.id_crc[stream_index]
                                    : nullptr;
      uint32_t *sequence_crc_output = &cp.read_info.sequence_crc[stream_index];
      uint32_t reads_in_step = read_fastq_block(
          input_streams[stream_index], id_array, read_array.data(),
          quality_array.empty() ? nullptr : quality_array.data(),
          num_reads_per_step, fasta_input, read_lengths_array.data(),
          contains_n_output,
          sequence_crc_output, // Compute CRC on original data before stripping
          quality_crc_output, id_crc_output, cp.encoding.preserve_quality);

      // Strip poly-A/T tails and accumulate per-read tail info.
      // We keep a local buffer (step_tail_info) indexed by read position in
      // this step so the parallel encoding section below can look it up.
      std::vector<uint16_t> step_tail_info;
      if (apply_poly_at && reads_in_step > 0) {
        step_tail_info.resize(reads_in_step, 0);
        bool any_stripped = false;
        // Strip serially — modifies read_array and read_lengths_array in place.
        for (uint32_t ri = 0; ri < reads_in_step; ++ri) {
          if (read_contains_N_array[ri] != 0) {
            continue;
          }
          const uint16_t strip_info =
              detect_and_strip_tail(read_array[ri], read_lengths_array[ri]);
          step_tail_info[ri] = strip_info;
          if ((strip_info >> 1) > 0) {
            any_stripped = true;
          }
        }
        if (any_stripped)
          cp.encoding.poly_at_stripped = true;
      }

      std::vector<uint8_t> step_atac_adapter_info;
      if (maybe_apply_atac_adapter_strip && reads_in_step > 0) {
        step_atac_adapter_info.resize(reads_in_step, 0);
        uint32_t eligible_reads = 0;
        uint64_t stripped_bases = 0;
        for (uint32_t ri = 0; ri < reads_in_step; ++ri) {
          if (read_contains_N_array[ri] != 0) {
            continue;
          }
          const uint8_t strip_info = detect_atac_adapter_tail_info(
              read_array[ri], read_lengths_array[ri]);
          step_atac_adapter_info[ri] = strip_info;
          eligible_reads++;
          stripped_bases += static_cast<uint64_t>(strip_info >> 1);
        }
        if (!atac_adapter_strip_decided) {
          const double avg_stripped_bases_per_read =
              eligible_reads == 0 ? 0.0
                                  : static_cast<double>(stripped_bases) /
                                        static_cast<double>(eligible_reads);
          atac_adapter_strip_active =
              avg_stripped_bases_per_read >= kMinAtacAdapterBasesPerRead;
          atac_adapter_strip_decided = true;
          SPRING_LOG_DEBUG(
              std::string("ATAC adapter stripping ") +
              (atac_adapter_strip_active ? "enabled" : "disabled") +
              " after sampling " + std::to_string(eligible_reads) +
              " reads (avg stripped bases/read=" +
              std::to_string(avg_stripped_bases_per_read) + ")");
        }
        if (atac_adapter_strip_active) {
          bool any_stripped = false;
          for (uint32_t ri = 0; ri < reads_in_step; ++ri) {
            const uint32_t strip_len = step_atac_adapter_info[ri] >> 1;
            if (strip_len == 0) {
              continue;
            }
            read_lengths_array[ri] -= strip_len;
            read_array[ri].resize(read_lengths_array[ri]);
            any_stripped = true;
          }
          if (any_stripped)
            cp.encoding.atac_adapter_stripped = true;
        } else {
          step_atac_adapter_info.clear();
        }
      }

      // Sequence CRC was already computed by read_fastq_block above on original
      // data, matching what decompression will compute after full restoration.

      if (maybe_strip_grouped_index_id_suffix && stream_index == 0 &&
          reads_in_step > 0) {
        if (!grouped_index_id_suffix_strip_decided) {
          grouped_index_id_suffix_strip_active = true;
          for (uint32_t ri = 0; ri < reads_in_step; ++ri) {
            std::string candidate_id = id_array[ri];
            if (!strip_grouped_index_suffix_from_id(
                    candidate_id, read_array[ri], cp.encoding.paired_end)) {
              grouped_index_id_suffix_strip_active = false;
              break;
            }
          }
          grouped_index_id_suffix_strip_decided = true;
          if (grouped_index_id_suffix_strip_active) {
            SPRING_LOG_DEBUG(
                "Grouped sc-RNA index-ID suffix stripping enabled "
                "(reconstruct trailing I1/I2 token from index reads)");
          }
        }

        if (grouped_index_id_suffix_strip_active) {
          for (uint32_t ri = 0; ri < reads_in_step; ++ri) {
            if (!strip_grouped_index_suffix_from_id(
                    id_array[ri], read_array[ri], cp.encoding.paired_end)) {
              throw std::runtime_error(
                  "Grouped sc-RNA index-ID suffix stripping became "
                  "inconsistent across reads.");
            }
          }
          cp.encoding.index_id_suffix_reconstructed = true;
        }
      }

      // Single-cell R1 prefix stripping must happen AFTER CRC computation but
      // BEFORE quality/read compression so that compressed data uses the
      // stripped lengths while integrity remains defined on full reads.
      if (apply_cb_strip && stream_index == 0 && reads_in_step > 0) {
        std::string cb_seq_buffer, cb_qual_buffer;
        cb_seq_buffer.reserve(reads_in_step * cp.encoding.cb_len);
        if (cp.encoding.preserve_quality) {
          cb_qual_buffer.reserve(reads_in_step * cp.encoding.cb_len);
        }
        for (uint32_t ri = 0; ri < reads_in_step; ++ri) {
          const std::string &read_seq = read_array[ri];
          if (read_seq.size() < cp.encoding.cb_len) {
            throw std::runtime_error(
                "CB prefix stripping requires all R1 reads to be at least " +
                std::to_string(cp.encoding.cb_len) + " bases long.");
          }
          const uint32_t strip_len = cp.encoding.cb_len;
          // Store CB sequence.
          cb_seq_buffer.append(read_seq.substr(0, strip_len));
          if (cp.encoding.preserve_quality) {
            const std::string &qual = quality_array[ri];
            cb_qual_buffer.append(qual.substr(0, strip_len));
            // Strip CB from quality.
            quality_array[ri] = qual.substr(strip_len);
          }
          // Strip CB from read sequence.
          read_array[ri] = read_seq.substr(strip_len);
          // Update read length to reflect stripped read.
          read_lengths_array[ri] = static_cast<uint16_t>(read_array[ri].size());
        }
        // Write CB data to output files.
        if (!cb_seq_buffer.empty()) {
          cb_seq_output.write(
              cb_seq_buffer.data(),
              static_cast<std::streamsize>(cb_seq_buffer.size()));
        }
        if (cp.encoding.preserve_quality && !cb_qual_buffer.empty()) {
          cb_qual_output.write(
              cb_qual_buffer.data(),
              static_cast<std::streamsize>(cb_qual_buffer.size()));
        }
        cp.encoding.cb_prefix_stripped = true;
        cp.encoding.cb_prefix_len = cp.encoding.cb_len;
      }

      SPRING_LOG_DEBUG(
          "Preprocess step: stream=" + std::to_string(stream_index + 1) +
          ", reads_in_step=" + std::to_string(reads_in_step) +
          ", blocks_done=" + std::to_string(num_blocks_done));
      if (reads_in_step < num_reads_per_step)
        done[stream_index] = true;
      if (reads_in_step == 0)
        continue;

      if (num_reads[0] + num_reads[1] + reads_in_step > MAX_NUM_READS) {
        std::cerr << "Max number of reads allowed is " << MAX_NUM_READS << "\n";
        throw std::runtime_error("Too many reads.");
      }
#pragma omp parallel
      {
        bool thread_done = false;
        uint64_t thread_id = omp_get_thread_num();
        if (stream_index == 1)
          paired_id_match_array[thread_id] = paired_id_match ? 1 : 0;
        if (thread_id * num_reads_per_block >= reads_in_step)
          thread_done = true;
        const uint32_t block_num =
            num_blocks_done + static_cast<uint32_t>(thread_id);
        const uint32_t thread_read_count = reads_for_thread_step(
            reads_in_step, num_reads_per_block, thread_id);
        std::ofstream read_length_output;
        if (!thread_done) {
          if (cp.encoding.long_flag)
            read_length_output.open(
                block_file_path(paths.read_length_paths[stream_index],
                                block_num),
                std::ios::binary);
          for (uint32_t read_index = thread_id * num_reads_per_block;
               read_index < thread_id * num_reads_per_block + thread_read_count;
               read_index++) {
            const uint32_t read_length = read_lengths_array[read_index];
            if (read_length > MAX_READ_LEN_LONG) {
              std::cerr << "Max read length for long mode is "
                        << MAX_READ_LEN_LONG << ", but found read of length "
                        << read_length << "\n";
              throw std::runtime_error("Too long read length");
            }

            if (cp.encoding.long_flag)
              read_length_output.write(
                  byte_ptr(&read_lengths_array[read_index]), sizeof(uint32_t));

            if (stream_index == 1 && paired_id_match_array[thread_id] &&
                !grouped_index_id_suffix_strip_active)
              paired_id_match_array[thread_id] =
                  check_id_pattern(id_array_1[read_index],
                                   id_array_2[read_index], paired_id_code);
          }
          if (cp.encoding.long_flag)
            read_length_output.close();
          if (cp.encoding.preserve_quality &&
              (cp.quality.ill_bin_flag || cp.quality.bin_thr_flag))
            quantize_quality(quality_array.data() +
                                 thread_id * num_reads_per_block,
                             thread_read_count, quality_binning_table.data());

          if (cp.encoding.preserve_quality && cp.quality.qvz_flag &&
              cp.encoding.preserve_order)
            quantize_quality_qvz(
                quality_array.data() + thread_id * num_reads_per_block,
                thread_read_count,
                read_lengths_array.data() + thread_id * num_reads_per_block,
                cp.quality.qvz_ratio);
          if (!cp.encoding.long_flag) {
            if (cp.encoding.preserve_order) {
              if (cp.encoding.preserve_id) {
                std::string output_path =
                    paths.id_output_paths[stream_index] + "." +
                    std::to_string(num_blocks_done + thread_id);
                compress_id_block(output_path.c_str(),
                                  id_array + thread_id * num_reads_per_block,
                                  thread_read_count,
                                  cp.encoding.compression_level);
              }
              if (cp.encoding.preserve_quality) {
                std::string output_path =
                    paths.quality_output_paths[stream_index] + "." +
                    std::to_string(num_blocks_done + thread_id);
                bsc::BSC_str_array_compress(
                    output_path.c_str(),
                    quality_array.data() + thread_id * num_reads_per_block,
                    thread_read_count,
                    read_lengths_array.data() +
                        thread_id * num_reads_per_block);
              }
            }
          } else {
            std::string read_length_input_path = block_file_path(
                paths.read_length_paths[stream_index], block_num);
            std::string read_length_output_path = compressed_block_file_path(
                paths.read_length_paths[stream_index], block_num);
            bsc::BSC_compress(read_length_input_path.c_str(),
                              read_length_output_path.c_str());
            remove(read_length_input_path.c_str());
            if (cp.encoding.preserve_id) {
              std::string output_path = compressed_block_file_path(
                  paths.id_output_paths[stream_index], block_num);
              compress_id_block(output_path.c_str(),
                                id_array + thread_id * num_reads_per_block,
                                thread_read_count,
                                cp.encoding.compression_level);
            }
            if (cp.encoding.preserve_quality) {
              std::string output_path = block_file_path(
                  paths.quality_output_paths[stream_index], block_num);
              std::string raw_quality_path = output_path + ".raw";
              write_raw_string_block(
                  raw_quality_path,
                  quality_array.data() + thread_id * num_reads_per_block,
                  thread_read_count,
                  read_lengths_array.data() + thread_id * num_reads_per_block);
              bsc::BSC_compress(raw_quality_path.c_str(), output_path.c_str());
              remove(raw_quality_path.c_str());
            }
            std::string output_path = block_file_path(
                paths.read_block_paths[stream_index], block_num);
            std::string compressed_output_path = compressed_block_file_path(
                paths.read_block_paths[stream_index], block_num);
            write_raw_string_block(
                output_path,
                read_array.data() + thread_id * num_reads_per_block,
                thread_read_count,
                read_lengths_array.data() + thread_id * num_reads_per_block);
            bsc::BSC_compress(output_path.c_str(),
                              compressed_output_path.c_str());
            remove(output_path.c_str());
          }
        }
      }
      if (cp.encoding.paired_end && (stream_index == 1)) {
        if (paired_id_match)
          for (int thread_id = 0; thread_id < cp.encoding.num_thr; thread_id++)
            paired_id_match &= paired_id_match_array[thread_id];
        if (!paired_id_match)
          paired_id_code = 0;
      }
      if (!cp.encoding.long_flag) {
        // Clear thread buffers for the next step (including tail bytes).
        for (short_read_thread_buffers &thread_buffer : short_read_buffers) {
          thread_buffer.clean_read_bytes.clear();
          thread_buffer.n_read_bytes.clear();
          thread_buffer.n_read_positions.clear();
          thread_buffer.tail_info_bytes.clear();
          thread_buffer.atac_adapter_bytes.clear();
          thread_buffer.clean_read_count = 0;
        }

#pragma omp parallel for schedule(static)
        for (int thread_id = 0; thread_id < cp.encoding.num_thr; thread_id++) {
          const uint32_t begin_read =
              static_cast<uint32_t>(thread_id) * num_reads_per_block;
          if (begin_read >= reads_in_step)
            continue;
          const uint32_t end_read =
              std::min(begin_read + num_reads_per_block, reads_in_step);

          short_read_thread_buffers &thread_buffer =
              short_read_buffers[static_cast<size_t>(thread_id)];
          const uint32_t thread_read_count = end_read - begin_read;
          thread_buffer.n_read_positions.reserve(thread_read_count / 2 + 1);

          for (uint32_t read_index = begin_read; read_index < end_read;
               read_index++) {
            if (read_contains_N_array[read_index] == 0) {
              append_encoded_dna_bits(thread_buffer.clean_read_bytes,
                                      read_array[read_index]);
              thread_buffer.clean_read_count++;
            } else {
              thread_buffer.n_read_positions.push_back(num_reads[stream_index] +
                                                       read_index);
              append_encoded_dna_n_bits(thread_buffer.n_read_bytes,
                                        read_array[read_index]);
            }
            // Record tail info for all reads (0 when stripping disabled or
            // N-read).
            if (apply_poly_at) {
              uint16_t info = 0;
              if (read_contains_N_array[read_index] == 0 &&
                  !step_tail_info.empty()) {
                info = step_tail_info[read_index];
                const uint32_t tail_len = info >> 1;
                if (tail_len > 0) {
                  // Also strip quality and record it.
                  std::string &qual = quality_array[read_index];
                  const std::string tail_qual =
                      qual.substr(qual.size() - tail_len);
                  qual.resize(qual.size() - tail_len);

                  append_uint16(thread_buffer.tail_info_bytes, info);
                  thread_buffer.tail_info_bytes.append(tail_qual);
                } else {
                  append_uint16(thread_buffer.tail_info_bytes, 0);
                }
              } else {
                append_uint16(thread_buffer.tail_info_bytes, 0);
              }
            }
            if (atac_adapter_strip_active) {
              uint8_t info = 0;
              if (read_contains_N_array[read_index] == 0 &&
                  !step_atac_adapter_info.empty()) {
                info = step_atac_adapter_info[read_index];
                std::string &qual = quality_array[read_index];
                const uint32_t strip_len = info >> 1;
                if (strip_len > 0) {
                  const std::string adapter_qual =
                      qual.substr(qual.size() - strip_len);
                  qual.resize(qual.size() - strip_len);
                  thread_buffer.atac_adapter_bytes.push_back(
                      static_cast<char>(info));
                  thread_buffer.atac_adapter_bytes.append(adapter_qual);
                } else {
                  thread_buffer.atac_adapter_bytes.push_back('\0');
                }
              } else {
                thread_buffer.atac_adapter_bytes.push_back('\0');
              }
            }
          }
        }

        num_reads_clean[stream_index] += flush_short_read_thread_buffers(
            short_read_buffers, clean_outputs[stream_index],
            n_read_outputs[stream_index], n_read_order_outputs[stream_index],
            apply_poly_at ? &tail_outputs[stream_index] : nullptr,
            atac_adapter_strip_active ? &atac_adapter_outputs[stream_index]
                                      : nullptr);
        if (!cp.encoding.preserve_order) {
          quality_chunk.clear();
          id_chunk.clear();
          if (quality_chunk.capacity() <
              static_cast<size_t>(reads_in_step) * 32U)
            quality_chunk.reserve(static_cast<size_t>(reads_in_step) * 32U);
          if (id_chunk.capacity() < static_cast<size_t>(reads_in_step) * 32U)
            id_chunk.reserve(static_cast<size_t>(reads_in_step) * 32U);
          for (uint32_t read_index = 0; read_index < reads_in_step;
               read_index++) {
            quality_chunk.append(quality_array[read_index]);
            quality_chunk.push_back('\n');
          }
          quality_outputs[stream_index].write(
              quality_chunk.data(),
              static_cast<std::streamsize>(quality_chunk.size()));

          for (uint32_t read_index = 0; read_index < reads_in_step;
               read_index++) {
            id_chunk.append(id_array[read_index]);
            id_chunk.push_back('\n');
          }
          id_outputs[stream_index].write(
              id_chunk.data(), static_cast<std::streamsize>(id_chunk.size()));
        }
      }
      num_reads[stream_index] += reads_in_step;
      max_readlen =
          std::max(max_readlen,
                   max_read_length_in_step(read_lengths_array, reads_in_step));
    }
    if (cp.encoding.paired_end)
      if (num_reads[0] != num_reads[1])
        throw std::runtime_error(
            "Number of reads in paired files do not match.");
    if (done[0] && done[1])
      break;
    num_blocks_done += cp.encoding.num_thr;
    if (progress && total_input_bytes > 0) {
      uint64_t bytes_read = 0;
      for (int i = 0; i < 2; i++) {
        if (input_streams[i]) {
          bytes_read += static_cast<uint64_t>(input_streams[i]->tellg());
        }
      }
      progress->update(static_cast<float>(bytes_read) / total_input_bytes);
    }
  }

  (void)0;
  // Close CB prefix streams before other cleanup.
  if (apply_cb_strip) {
    if (cb_seq_output.is_open())
      cb_seq_output.close();
    if (cb_qual_output.is_open())
      cb_qual_output.close();

    // Compress CB files with BSC to reduce archive size.
    const std::string cb_seq_path = temp_dir + "/cb_prefix.dna";
    const std::string cb_seq_compressed = cb_seq_path + ".bsc";
    if (std::filesystem::exists(cb_seq_path)) {
      bsc::BSC_compress(cb_seq_path.c_str(), cb_seq_compressed.c_str());
      remove(cb_seq_path.c_str());
    }
    if (cp.encoding.preserve_quality) {
      const std::string cb_qual_path = temp_dir + "/cb_prefix.qual";
      const std::string cb_qual_compressed = cb_qual_path + ".bsc";
      if (std::filesystem::exists(cb_qual_path)) {
        bsc::BSC_compress(cb_qual_path.c_str(), cb_qual_compressed.c_str());
        remove(cb_qual_path.c_str());
      }
    }
  }
  if (maybe_apply_atac_adapter_strip) {
    for (int stream_index = 0; stream_index < 2; ++stream_index) {
      if (stream_index == 1 && !cp.encoding.paired_end)
        continue;
      if (atac_adapter_outputs[stream_index].is_open())
        atac_adapter_outputs[stream_index].close();

      const std::string adapter_path = temp_dir + "/atac_adapter_" +
                                       std::to_string(stream_index + 1) +
                                       ".bin";
      const std::string adapter_compressed = adapter_path + ".bsc";
      if (!cp.encoding.atac_adapter_stripped) {
        if (std::filesystem::exists(adapter_path))
          remove(adapter_path.c_str());
        if (std::filesystem::exists(adapter_compressed))
          remove(adapter_compressed.c_str());
        continue;
      }
      if (std::filesystem::exists(adapter_path)) {
        bsc::BSC_compress(adapter_path.c_str(), adapter_compressed.c_str());
        remove(adapter_path.c_str());
      }
    }
  }
  close_preprocess_streams(input_files, clean_outputs, n_read_outputs,
                           n_read_order_outputs, id_outputs, quality_outputs,
                           input_streams, gzip_streams, cp, false);
  if (num_reads[0] == 0)
    throw std::runtime_error("No reads found.");

  if (!cp.encoding.long_flag && cp.encoding.paired_end) {
    // Shift mate-2 N-read positions by file-1 length before merging the
    // streams.
    merge_paired_n_reads(paths, num_reads, num_reads_clean);
  }

  remove_redundant_mate_ids(paths, cp, paired_id_match, num_reads,
                            num_reads_per_block);
  cp.read_info.paired_id_code = paired_id_code;
  cp.read_info.paired_id_match = paired_id_match;
  cp.read_info.quality_header_has_id = quality_header_has_id;
  cp.read_info.num_reads = num_reads[0] + num_reads[1];
  cp.read_info.num_reads_clean[0] = num_reads_clean[0];
  cp.read_info.num_reads_clean[1] = num_reads_clean[1];
  cp.read_info.max_readlen = max_readlen;

  SPRING_LOG_DEBUG(
      "Preprocess complete: num_reads=" +
      std::to_string(cp.read_info.num_reads) +
      ", num_reads_clean_1=" + std::to_string(cp.read_info.num_reads_clean[0]) +
      ", num_reads_clean_2=" + std::to_string(cp.read_info.num_reads_clean[1]) +
      ", max_readlen=" + std::to_string(cp.read_info.max_readlen) +
      ", sequence_crc_1=" + std::to_string(cp.read_info.sequence_crc[0]) +
      ", sequence_crc_2=" + std::to_string(cp.read_info.sequence_crc[1]) +
      ", quality_crc_1=" + std::to_string(cp.read_info.quality_crc[0]) +
      ", quality_crc_2=" + std::to_string(cp.read_info.quality_crc[1]) +
      ", id_crc_1=" + std::to_string(cp.read_info.id_crc[0]) +
      ", id_crc_2=" + std::to_string(cp.read_info.id_crc[1]));

  SPRING_LOG_INFO("Max Read length: " +
                  std::to_string(cp.read_info.max_readlen));
  SPRING_LOG_INFO("Total number of reads: " +
                  std::to_string(cp.read_info.num_reads));

  if (cp.encoding.paired_end) {
    SPRING_LOG_INFO("Total number of reads without N: " +
                    std::to_string(cp.read_info.num_reads_clean[0] +
                                   cp.read_info.num_reads_clean[1]));
    SPRING_LOG_INFO("Paired id match code: " +
                    std::to_string((int)cp.read_info.paired_id_code));
  }
}

} // namespace spring
