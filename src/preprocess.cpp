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

#include "preprocess.h"
#include <algorithm>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <omp.h>
#include <stdexcept>
#include <string>
#include <vector>

#include "libbsc/bsc.h"
#include "params.h"
#include "util.h"

namespace spring {

namespace {

using gzip_input_buffer =
    boost::iostreams::filtering_streambuf<boost::iostreams::input>;

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

void open_input_stream(std::ifstream &file_stream, std::istream *&input_stream,
                       gzip_input_buffer *&gzip_buffer,
                       const std::string &path, const bool gzip_enabled) {
  if (gzip_enabled) {
    file_stream.open(path, std::ios_base::binary);
    gzip_buffer = new gzip_input_buffer;
    gzip_buffer->push(boost::iostreams::gzip_decompressor());
    gzip_buffer->push(file_stream);
    input_stream = new std::istream(gzip_buffer);
    return;
  }

  file_stream.open(path);
  input_stream = &file_stream;
  gzip_buffer = nullptr;
}

void reset_input_stream(std::ifstream &file_stream, std::istream *&input_stream,
                        gzip_input_buffer *&gzip_buffer,
                        const std::string &path, const bool gzip_enabled) {
  if (gzip_enabled) {
    file_stream.close();
    delete input_stream;
    delete gzip_buffer;
    input_stream = nullptr;
    gzip_buffer = nullptr;
    open_input_stream(file_stream, input_stream, gzip_buffer, path, true);
    return;
  }

  file_stream.clear();
  file_stream.seekg(0);
}

void close_input_stream(std::ifstream &file_stream, std::istream *&input_stream,
                        gzip_input_buffer *&gzip_buffer,
                        const bool gzip_enabled) {
  if (gzip_enabled) {
    delete input_stream;
    delete gzip_buffer;
    input_stream = nullptr;
    gzip_buffer = nullptr;
  }
  file_stream.close();
}

} // namespace

void preprocess(const std::string &infile_1, const std::string &infile_2,
                const std::string &temp_dir, compression_params &cp,
                const bool &gzip_flag, const bool &fasta_flag) {
  std::string input_paths[2] = {infile_1, infile_2};
  std::string clean_read_paths[2];
  std::string n_read_paths[2];
  std::string n_read_order_paths[2];
  std::string id_output_paths[2];
  std::string quality_output_paths[2];
  std::string read_block_paths[2];
  std::string read_length_paths[2];
  std::string base_dir = temp_dir;
  clean_read_paths[0] = base_dir + "/input_clean_1.dna";
  clean_read_paths[1] = base_dir + "/input_clean_2.dna";
  n_read_paths[0] = base_dir + "/input_N.dna";
  n_read_paths[1] = base_dir + "/input_N.dna.2";
  n_read_order_paths[0] = base_dir + "/read_order_N.bin";
  n_read_order_paths[1] = base_dir + "/read_order_N.bin.2";
  id_output_paths[0] = base_dir + "/id_1";
  id_output_paths[1] = base_dir + "/id_2";
  quality_output_paths[0] = base_dir + "/quality_1";
  quality_output_paths[1] = base_dir + "/quality_2";
  read_block_paths[0] = base_dir + "/read_1";
  read_block_paths[1] = base_dir + "/read_2";
  read_length_paths[0] = base_dir + "/readlength_1";
  read_length_paths[1] = base_dir + "/readlength_2";

  std::ifstream input_files[2];
  std::ofstream clean_outputs[2];
  std::ofstream n_read_outputs[2];
  std::ofstream n_read_order_outputs[2];
  std::ofstream id_outputs[2];
  std::ofstream quality_outputs[2];
  std::istream *input_streams[2] = {nullptr, nullptr};
  gzip_input_buffer *gzip_buffers[2] = {nullptr, nullptr};

  for (int stream_index = 0; stream_index < 2; stream_index++) {
    if (stream_index == 1 && !cp.paired_end)
      continue;
    open_input_stream(input_files[stream_index], input_streams[stream_index],
                      gzip_buffers[stream_index], input_paths[stream_index],
                      gzip_flag);
    if (!cp.long_flag) {
      clean_outputs[stream_index].open(clean_read_paths[stream_index],
                                       std::ios::binary);
      n_read_outputs[stream_index].open(n_read_paths[stream_index],
                                        std::ios::binary);
      n_read_order_outputs[stream_index].open(
          n_read_order_paths[stream_index], std::ios::binary);
      if (!cp.preserve_order) {
        if (cp.preserve_id)
          id_outputs[stream_index].open(id_output_paths[stream_index]);
        if (cp.preserve_quality)
          quality_outputs[stream_index].open(
              quality_output_paths[stream_index]);
      }
    }
  }

  uint32_t max_readlen = 0;
  uint64_t num_reads[2] = {0, 0};
  uint64_t num_reads_clean[2] = {0, 0};
  uint32_t num_reads_per_block;
  if (!cp.long_flag)
    num_reads_per_block = cp.num_reads_per_block;
  else
    num_reads_per_block = cp.num_reads_per_block_long;
  uint8_t paired_id_code = 0;
  bool paired_id_match = false;

  char *quality_binning_table = new char[128];
  if (cp.ill_bin_flag)
    generate_illumina_binning_table(quality_binning_table);
  if (cp.bin_thr_flag)
    generate_binary_binning_table(quality_binning_table, cp.bin_thr_thr,
                                  cp.bin_thr_high, cp.bin_thr_low);

  if (!input_files[0].is_open())
    throw std::runtime_error("Error opening input file");
  if (cp.paired_end) {
    if (!input_files[1].is_open())
      throw std::runtime_error("Error opening input file");
    if (cp.preserve_id) {
      // Probe the mate-id pattern once so later stages can drop redundant ids.
      std::string id_1, id_2;
      std::getline(*input_streams[0], id_1);
      std::getline(*input_streams[1], id_2);
      paired_id_code = find_id_pattern(id_1, id_2);
      if (paired_id_code != 0)
        paired_id_match = true;
      for (int stream_index = 0; stream_index < 2; stream_index++)
        reset_input_stream(input_files[stream_index],
                           input_streams[stream_index],
                           gzip_buffers[stream_index],
                           input_paths[stream_index], gzip_flag);
    }
  }
  if (cp.num_thr <= 0)
    throw std::runtime_error("Number of threads must be positive.");

  uint64_t num_reads_per_step = (uint64_t)cp.num_thr * num_reads_per_block;
  std::vector<std::string> read_array(num_reads_per_step);
  std::vector<std::string> id_array_1(num_reads_per_step);
  std::vector<std::string> id_array_2(num_reads_per_step);
  std::vector<std::string> quality_array(num_reads_per_step);
  std::vector<bool> read_contains_N_array(num_reads_per_step);
  std::vector<uint32_t> read_lengths_array(num_reads_per_step);
  std::vector<bool> paired_id_match_array(static_cast<size_t>(cp.num_thr));

  omp_set_num_threads(cp.num_thr);

  uint32_t num_blocks_done = 0;

  while (true) {
    bool done[2] = {true, true};
    for (int stream_index = 0; stream_index < 2; stream_index++) {
      if (stream_index == 1 && !cp.paired_end)
        continue;
      done[stream_index] = false;
      std::string *id_array =
          (stream_index == 0) ? id_array_1.data() : id_array_2.data();
      uint32_t reads_in_step =
          read_fastq_block(input_streams[stream_index], id_array,
                           read_array.data(),
                           quality_array.data(),
                           num_reads_per_step, fasta_flag);
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
          paired_id_match_array[thread_id] = paired_id_match;
        if (thread_id * num_reads_per_block >= reads_in_step)
          thread_done = true;
        const uint32_t block_num =
            num_blocks_done + static_cast<uint32_t>(thread_id);
        uint32_t thread_read_count =
            std::min((uint64_t)reads_in_step,
                     (thread_id + 1) * num_reads_per_block) -
            thread_id * num_reads_per_block;
        std::ofstream read_length_output;
        if (!thread_done) {
          if (cp.long_flag)
            read_length_output.open(
                block_file_path(read_length_paths[stream_index], block_num),
                std::ios::binary);
          for (uint32_t read_index = thread_id * num_reads_per_block;
               read_index < thread_id * num_reads_per_block + thread_read_count;
               read_index++) {
            size_t read_length = read_array[read_index].size();
            if (cp.long_flag && read_length > MAX_READ_LEN_LONG) {
              std::cerr << "Max read length for long mode is "
                        << MAX_READ_LEN_LONG << ", but found read of length "
                        << read_length << "\n";
              throw std::runtime_error("Too long read length");
            }
            if (!cp.long_flag && read_length > MAX_READ_LEN) {
              std::cerr << "Max read length without long mode is "
                        << MAX_READ_LEN << ", but found read of length "
                        << read_length << "\n";
              throw std::runtime_error(
                  "Too long read length (please try --long/-l flag).");
            }
            if (cp.preserve_quality &&
                (quality_array[read_index].size() != read_length))
              throw std::runtime_error(
                  "Read length does not match quality length.");
            read_lengths_array[read_index] = (uint32_t)read_length;

            if (!cp.long_flag)
              read_contains_N_array[read_index] =
                  (read_array[read_index].find('N') != std::string::npos);

            if (cp.long_flag)
              read_length_output.write(
                  byte_ptr(&read_lengths_array[read_index]),
                  sizeof(uint32_t));

            if (stream_index == 1 && paired_id_match_array[thread_id])
              paired_id_match_array[thread_id] = check_id_pattern(
                  id_array_1[read_index], id_array_2[read_index],
                  paired_id_code);
          }
          if (cp.long_flag)
            read_length_output.close();
          if (cp.preserve_quality && (cp.ill_bin_flag || cp.bin_thr_flag))
            quantize_quality(
                quality_array.data() + thread_id * num_reads_per_block,
                thread_read_count, quality_binning_table);

          if (cp.preserve_quality && cp.qvz_flag && cp.preserve_order)
            quantize_quality_qvz(
                quality_array.data() + thread_id * num_reads_per_block,
                thread_read_count,
                read_lengths_array.data() + thread_id * num_reads_per_block,
                cp.qvz_ratio);
          if (!cp.long_flag) {
            if (cp.preserve_order) {
              if (cp.preserve_id) {
                std::string output_path = id_output_paths[stream_index] + "." +
                                          std::to_string(num_blocks_done +
                                                         thread_id);
                compress_id_block(output_path.c_str(),
                                  id_array + thread_id * num_reads_per_block,
                                  thread_read_count);
              }
              if (cp.preserve_quality) {
                std::string output_path =
                    quality_output_paths[stream_index] + "." +
                    std::to_string(num_blocks_done + thread_id);
                bsc::BSC_str_array_compress(
                    output_path.c_str(),
                    quality_array.data() + thread_id * num_reads_per_block,
                    thread_read_count,
                    read_lengths_array.data() + thread_id * num_reads_per_block);
              }
            }
          } else {
            std::string read_length_input_path =
                block_file_path(read_length_paths[stream_index], block_num);
            std::string read_length_output_path =
                read_length_input_path + ".bsc";
            bsc::BSC_compress(read_length_input_path.c_str(),
                              read_length_output_path.c_str());
            remove(read_length_input_path.c_str());
            if (cp.preserve_id) {
              std::string output_path =
                  block_file_path(id_output_paths[stream_index], block_num);
              compress_id_block(output_path.c_str(),
                                id_array + thread_id * num_reads_per_block,
                                thread_read_count);
            }
            if (cp.preserve_quality) {
              std::string output_path =
                  block_file_path(quality_output_paths[stream_index],
                                  block_num);
              bsc::BSC_str_array_compress(
                  output_path.c_str(),
                  quality_array.data() + thread_id * num_reads_per_block,
                  thread_read_count,
                  read_lengths_array.data() + thread_id * num_reads_per_block);
            }
            std::string output_path =
                block_file_path(read_block_paths[stream_index], block_num);
            bsc::BSC_str_array_compress(
                output_path.c_str(),
                read_array.data() + thread_id * num_reads_per_block,
                thread_read_count,
                read_lengths_array.data() + thread_id * num_reads_per_block);
          }
        }
      }
      if (cp.paired_end && (stream_index == 1)) {
        if (paired_id_match)
          for (int thread_id = 0; thread_id < cp.num_thr; thread_id++)
            paired_id_match &= paired_id_match_array[thread_id];
        if (!paired_id_match)
          paired_id_code = 0;
      }
      if (!cp.long_flag) {
        for (uint32_t read_index = 0; read_index < reads_in_step;
             read_index++) {
          if (!read_contains_N_array[read_index]) {
            write_dna_in_bits(read_array[read_index],
                              clean_outputs[stream_index]);
            num_reads_clean[stream_index]++;
          } else {
            uint32_t n_read_position = num_reads[stream_index] + read_index;
            n_read_order_outputs[stream_index].write(
                byte_ptr(&n_read_position), sizeof(uint32_t));
            write_dnaN_in_bits(read_array[read_index],
                               n_read_outputs[stream_index]);
          }
        }
        if (!cp.preserve_order) {
          for (uint32_t read_index = 0; read_index < reads_in_step;
               read_index++)
            quality_outputs[stream_index] << quality_array[read_index]
                                          << "\n";
          for (uint32_t read_index = 0; read_index < reads_in_step;
               read_index++)
            id_outputs[stream_index] << id_array[read_index] << "\n";
        }
      }
      num_reads[stream_index] += reads_in_step;
      max_readlen =
          std::max(max_readlen,
               *(std::max_element(read_lengths_array.begin(),
                        read_lengths_array.begin() +
                          reads_in_step)));
    }
    if (cp.paired_end)
      if (num_reads[0] != num_reads[1])
        throw std::runtime_error(
            "Number of reads in paired files do not match.");
    if (done[0] && done[1])
      break;
    num_blocks_done += cp.num_thr;
  }

  delete[] quality_binning_table;
  for (int stream_index = 0; stream_index < 2; stream_index++) {
    if (stream_index == 1 && !cp.paired_end)
      continue;
    close_input_stream(input_files[stream_index], input_streams[stream_index],
                       gzip_buffers[stream_index], gzip_flag);
    if (!cp.long_flag) {
      clean_outputs[stream_index].close();
      n_read_outputs[stream_index].close();
      n_read_order_outputs[stream_index].close();
      if (!cp.preserve_order) {
        if (cp.preserve_id)
          id_outputs[stream_index].close();
        if (cp.preserve_quality)
          quality_outputs[stream_index].close();
      }
    }
  }
  if (num_reads[0] == 0)
    throw std::runtime_error("No reads found.");

  if (!cp.long_flag && cp.paired_end) {
    // Shift mate-2 N-read positions by file-1 length before merging the streams.
    std::ofstream merged_n_read_output(n_read_paths[0],
                                       std::ios::app | std::ios::binary);
    std::ifstream mate_n_read_input(n_read_paths[1], std::ios::binary);
    merged_n_read_output << mate_n_read_input.rdbuf();
    merged_n_read_output.close();
    mate_n_read_input.close();
    remove(n_read_paths[1].c_str());
    std::ofstream merged_n_read_order_output(n_read_order_paths[0],
                                             std::ios::app | std::ios::binary);
    std::ifstream mate_n_read_order_input(n_read_order_paths[1],
                                          std::ios::binary);
    uint32_t mate_n_read_count = num_reads[1] - num_reads_clean[1];
    uint32_t n_read_order;
    for (uint32_t read_index = 0; read_index < mate_n_read_count;
         read_index++) {
      mate_n_read_order_input.read(byte_ptr(&n_read_order), sizeof(uint32_t));
      n_read_order += num_reads[0];
      merged_n_read_order_output.write(byte_ptr(&n_read_order),
                                       sizeof(uint32_t));
    }
    mate_n_read_order_input.close();
    merged_n_read_order_output.close();
    remove(n_read_order_paths[1].c_str());
  }

  if (cp.paired_end && paired_id_match) {
    if (!cp.long_flag && !cp.preserve_order) {
      remove(id_output_paths[1].c_str());
    } else {
      const uint32_t num_blocks = block_count(num_reads[0], num_reads_per_block);
      for (uint32_t block_index = 0; block_index < num_blocks; block_index++)
        remove(block_file_path(id_output_paths[1], block_index).c_str());
    }
  }
  cp.paired_id_code = paired_id_code;
  cp.paired_id_match = paired_id_match;
  cp.num_reads = num_reads[0] + num_reads[1];
  cp.num_reads_clean[0] = num_reads_clean[0];
  cp.num_reads_clean[1] = num_reads_clean[1];
  cp.max_readlen = max_readlen;

  std::cout << "Max Read length: " << cp.max_readlen << "\n";
  std::cout << "Total number of reads: " << cp.num_reads << "\n";

  if (!cp.long_flag)
    std::cout << "Total number of reads without N: "
              << cp.num_reads_clean[0] + cp.num_reads_clean[1] << "\n";
  if (cp.preserve_id && cp.paired_end)
    std::cout << "Paired id match code: " << (int)cp.paired_id_code << "\n";
}

} // namespace spring
