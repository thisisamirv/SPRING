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
#include <vector>

#include "core_utils.h"
#include "dna_utils.h"
#include "integrity_utils.h"
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

std::string block_file_path(const std::string &base_path,
                            const uint32_t block_num) {
  return base_path + "." + std::to_string(block_num);
}

uint32_t block_count(const uint64_t num_reads,
                     const uint32_t num_reads_per_block) {
  if (num_reads == 0)
    return 0;
  return 1 + (num_reads - 1) / num_reads_per_block;
}

void write_raw_string_block(const std::string &output_path,
                            std::string *strings,
                            const uint32_t string_count,
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
    remove(block_file_path(paths.id_output_paths[1], block_index).c_str());
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

uint32_t detect_max_read_length_in_file(const std::string &path,
                                        bool fasta_input, bool &use_crlf) {
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
                                bool &use_crlf) {
  SPRING_LOG_INFO("Auto-detecting read lengths and line endings ...");
  use_crlf = false;
  uint32_t max_len_1 =
      detect_max_read_length_in_file(infile_1, fasta_input, use_crlf);
  uint32_t max_len_2 = 0;
  if (paired_end) {
    max_len_2 = detect_max_read_length_in_file(infile_2, fasta_input, use_crlf);
  }
  return std::max(max_len_1, max_len_2);
}

void preprocess(const std::string &infile_1, const std::string &infile_2,
                const std::string &temp_dir, compression_params &cp,
                const bool &fasta_input, ProgressBar *progress) {
  SPRING_LOG_DEBUG("Preprocess start: temp_dir=" + temp_dir +
                    ", input1=" + infile_1 +
                    (cp.encoding.paired_end ? (", input2=" + infile_2)
                                           : std::string()) +
                    ", long_mode=" +
                    std::string(cp.encoding.long_flag ? "true" : "false") +
                    ", paired_end=" +
                    std::string(cp.encoding.paired_end ? "true" : "false") +
                    ", preserve_order=" +
                    std::string(cp.encoding.preserve_order ? "true" : "false") +
                    ", preserve_id=" +
                    std::string(cp.encoding.preserve_id ? "true" : "false") +
                    ", preserve_quality=" +
                    std::string(cp.encoding.preserve_quality ? "true" : "false") +
                    ", fasta_input=" +
                    std::string(fasta_input ? "true" : "false"));
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
    SPRING_LOG_DEBUG("Paired ID pattern detection: code=" +
                      std::to_string(static_cast<int>(paired_id_code)) +
                      ", match=" +
                      std::string(paired_id_match ? "true" : "false"));
  }

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
  std::vector<std::string> id_array_2(num_reads_per_step);
  std::vector<std::string> quality_array(num_reads_per_step);
    std::vector<uint8_t> read_contains_N_array(num_reads_per_step, 0);
  std::vector<uint32_t> read_lengths_array(num_reads_per_step);
    std::vector<uint8_t> paired_id_match_array(
      static_cast<size_t>(cp.encoding.num_thr), 0);

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
      uint32_t reads_in_step = read_fastq_block(
          input_streams[stream_index], id_array, read_array.data(),
          quality_array.data(), num_reads_per_step, fasta_input);
        SPRING_LOG_DEBUG("Preprocess step: stream=" +
                std::to_string(stream_index + 1) +
                ", reads_in_step=" + std::to_string(reads_in_step) +
                ", blocks_done=" + std::to_string(num_blocks_done));
      if (reads_in_step < num_reads_per_step)
        done[stream_index] = true;
      if (reads_in_step == 0)
        continue;

      // Update integrity digests
      for (uint32_t i = 0; i < reads_in_step; ++i) {
        update_record_crc(cp.read_info.sequence_crc[stream_index],
                          read_array[i]);
        if (cp.encoding.preserve_quality) {
          update_record_crc(cp.read_info.quality_crc[stream_index],
                            quality_array[i]);
        }
        if (cp.encoding.preserve_id) {
          update_record_crc(cp.read_info.id_crc[stream_index], id_array[i]);
        }
      }

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
            size_t read_length = read_array[read_index].size();
            if (read_length > MAX_READ_LEN_LONG) {
              std::cerr << "Max read length for long mode is "
                        << MAX_READ_LEN_LONG << ", but found read of length "
                        << read_length << "\n";
              throw std::runtime_error("Too long read length");
            }
            if (cp.encoding.preserve_quality &&
                (quality_array[read_index].size() != read_length))
              throw std::runtime_error(
                  "Read length does not match quality length.");
            read_lengths_array[read_index] = (uint32_t)read_length;

            if (!cp.encoding.long_flag)
              read_contains_N_array[read_index] =
                  (read_array[read_index].find('N') != std::string::npos);

            if (cp.encoding.long_flag)
              read_length_output.write(
                  byte_ptr(&read_lengths_array[read_index]), sizeof(uint32_t));

            if (stream_index == 1 && paired_id_match_array[thread_id])
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
                                  thread_read_count, cp.encoding.compression_level);
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
            std::string read_length_output_path =
                read_length_input_path + ".bsc";
            std::filesystem::copy_file(
              read_length_input_path, read_length_output_path,
              std::filesystem::copy_options::overwrite_existing);
            remove(read_length_input_path.c_str());
            if (cp.encoding.preserve_id) {
              std::string output_path = block_file_path(
                  paths.id_output_paths[stream_index], block_num);
              compress_id_block(output_path.c_str(),
                                id_array + thread_id * num_reads_per_block,
                                thread_read_count,
                                cp.encoding.compression_level,
                                true);
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
              bsc::BSC_compress(raw_quality_path.c_str(),
                      output_path.c_str());
              remove(raw_quality_path.c_str());
            }
            std::string output_path = block_file_path(
                paths.read_block_paths[stream_index], block_num);
            write_raw_string_block(
                output_path,
                read_array.data() + thread_id * num_reads_per_block,
                thread_read_count,
                read_lengths_array.data() + thread_id * num_reads_per_block);
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
        // Parallelize N-read classification
        std::vector<uint8_t> is_n_read(reads_in_step, 0);
        uint32_t thread_local_clean_count = 0;

#pragma omp parallel for schedule(static) reduction(+:thread_local_clean_count)
        for (uint32_t read_index = 0; read_index < reads_in_step;
             read_index++) {
          is_n_read[read_index] = read_contains_N_array[read_index];
          if (!is_n_read[read_index]) {
            thread_local_clean_count++;
          }
        }

        // Write encoded reads sequentially to avoid file stream corruption
        for (uint32_t read_index = 0; read_index < reads_in_step;
             read_index++) {
          if (!is_n_read[read_index]) {
            write_dna_in_bits(read_array[read_index],
                              clean_outputs[stream_index]);
          } else {
            uint32_t n_read_position = num_reads[stream_index] + read_index;
            n_read_order_outputs[stream_index].write(byte_ptr(&n_read_position),
                                                     sizeof(uint32_t));
            write_dnaN_in_bits(read_array[read_index],
                               n_read_outputs[stream_index]);
          }
        }
        num_reads_clean[stream_index] += thread_local_clean_count;

        if (!cp.encoding.preserve_order) {
          for (uint32_t read_index = 0; read_index < reads_in_step;
               read_index++)
            quality_outputs[stream_index] << quality_array[read_index] << "\n";
          for (uint32_t read_index = 0; read_index < reads_in_step;
               read_index++)
            id_outputs[stream_index] << id_array[read_index] << "\n";
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
  cp.read_info.num_reads = num_reads[0] + num_reads[1];
  cp.read_info.num_reads_clean[0] = num_reads_clean[0];
  cp.read_info.num_reads_clean[1] = num_reads_clean[1];
  cp.read_info.max_readlen = max_readlen;

  SPRING_LOG_DEBUG(
      "Preprocess complete: num_reads=" + std::to_string(cp.read_info.num_reads) +
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

