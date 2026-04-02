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

#include "decompress.h"
#include "libbsc/bsc.h"
#include "util.h"
#include <cstdint>
#include <fstream>
#include <iostream>
#include <omp.h>
#include <stdexcept>
#include <string>

namespace spring {

namespace {

std::string block_file_path(const std::string &base_path,
                            const uint32_t block_num) {
  return base_path + '.' + std::to_string(block_num);
}

std::string compressed_block_file_path(const std::string &base_path,
                                       const uint32_t block_num) {
  return block_file_path(base_path, block_num) + ".bsc";
}

void decompress_bsc_block(const std::string &base_path, const uint32_t block_num) {
  const std::string output_path = block_file_path(base_path, block_num);
  const std::string input_path = compressed_block_file_path(base_path, block_num);
  bsc::BSC_decompress(input_path.c_str(), output_path.c_str());
  remove(input_path.c_str());
}

void open_output_files(std::ofstream (&output_streams)[2],
                       const std::string (&output_paths)[2],
                       const bool paired_end, const bool gzip_flag) {
  for (int stream_index = 0; stream_index < 2; stream_index++) {
    if (stream_index == 1 && !paired_end)
      continue;
    if (gzip_flag)
      output_streams[stream_index].open(output_paths[stream_index],
                                        std::ios::binary);
    else
      output_streams[stream_index].open(output_paths[stream_index]);
  }
}

void validate_output_files(std::ofstream (&output_streams)[2],
                           const bool paired_end) {
  if (!output_streams[0].is_open())
    throw std::runtime_error("Error opening output file");
  if (paired_end && !output_streams[1].is_open())
    throw std::runtime_error("Error opening output file");
}

uint64_t compute_num_reads_per_step(const uint32_t num_reads,
                                    const uint32_t num_reads_per_block,
                                    const int num_thr,
                                    const bool paired_end) {
  uint64_t num_reads_per_step =
      static_cast<uint64_t>(num_thr) * num_reads_per_block;
  const uint64_t total_reads = paired_end ? num_reads / 2 : num_reads;
  if (num_reads_per_step > total_reads)
    num_reads_per_step = total_reads;
  return num_reads_per_step;
}

uint32_t compute_num_reads_cur_step(const uint32_t num_reads,
                                    const uint32_t num_reads_done,
                                    const uint64_t num_reads_per_step,
                                    const bool paired_end) {
  const uint32_t total_reads = paired_end ? num_reads / 2 : num_reads;
  if (num_reads_done + num_reads_per_step >= total_reads)
    return total_reads - num_reads_done;
  return static_cast<uint32_t>(num_reads_per_step);
}

} // namespace

void set_dec_noise_array(char **dec_noise);

void decompress_short(const std::string &temp_dir, const std::string &outfile_1,
                      const std::string &outfile_2,
                      const compression_params &cp, const int &num_thr,
                      const uint64_t &start_num, const uint64_t &end_num,
                      const bool &gzip_flag, const int &gzip_level) {
  std::string base_dir = temp_dir;

  std::string file_seq = base_dir + "/read_seq.bin";
  std::string file_flag = base_dir + "/read_flag.txt";
  std::string file_pos = base_dir + "/read_pos.bin";
  std::string file_pos_pair = base_dir + "/read_pos_pair.bin";
  std::string file_RC = base_dir + "/read_rev.txt";
  std::string file_RC_pair = base_dir + "/read_rev_pair.txt";
  std::string file_readlength = base_dir + "/read_lengths.bin";
  std::string file_unaligned = base_dir + "/read_unaligned.txt";
  std::string file_noise = base_dir + "/read_noise.txt";
  std::string file_noisepos = base_dir + "/read_noisepos.bin";
  std::string input_quality_paths[2];
  std::string input_id_paths[2];

  input_quality_paths[0] = base_dir + "/quality_1";
  input_quality_paths[1] = base_dir + "/quality_2";
  input_id_paths[0] = base_dir + "/id_1";
  input_id_paths[1] = base_dir + "/id_2";

  uint32_t num_reads = cp.num_reads;
  uint8_t paired_id_code = cp.paired_id_code;
  bool paired_id_match = cp.paired_id_match;
  uint32_t num_reads_per_block = cp.num_reads_per_block;
  bool paired_end = cp.paired_end;
  bool preserve_id = cp.preserve_id;
  bool preserve_quality = cp.preserve_quality;
  bool preserve_order = cp.preserve_order;

  std::string output_paths[2] = {outfile_1, outfile_2};
  std::ofstream output_streams[2];

  open_output_files(output_streams, output_paths, paired_end, gzip_flag);
  validate_output_files(output_streams, paired_end);

  const uint64_t num_reads_per_step =
      compute_num_reads_per_step(num_reads, num_reads_per_block, num_thr,
                                 paired_end);

  std::string *read_buffer_1 = new std::string[num_reads_per_step];
  std::string *read_buffer_2 = NULL;
  if (paired_end)
    read_buffer_2 = new std::string[num_reads_per_step];
  std::string *id_buffer = new std::string[num_reads_per_step];
  std::string *quality_buffer = NULL;
  if (preserve_quality)
    quality_buffer = new std::string[num_reads_per_step];
  uint32_t *read_lengths_buffer_1 = new uint32_t[num_reads_per_step];
  uint32_t *read_lengths_buffer_2 = NULL;
  if (paired_end)
    read_lengths_buffer_2 = new uint32_t[num_reads_per_step];
  char **decoded_noise_table;
  decoded_noise_table = new char *[128];
  for (int i = 0; i < 128; i++)
    decoded_noise_table[i] = new char[128];
  set_dec_noise_array(decoded_noise_table);

  omp_set_num_threads(num_thr);

  // Rebuild the packed reference sequence once before block processing.
  std::string seq;
  int encoding_thread_count = cp.num_thr; // number of encoding threads
  decompress_unpack_seq(file_seq, encoding_thread_count, num_thr);
  for (int encoding_thread_id = 0; encoding_thread_id < encoding_thread_count;
       encoding_thread_id++) {
    uint64_t prev_len = seq.size();
    uint64_t sequence_chunk_size;
    std::string unpacked_seq_path =
        file_seq + '.' + std::to_string(encoding_thread_id);
    std::ifstream in_seq(unpacked_seq_path);
    in_seq.seekg(0, in_seq.end);
    sequence_chunk_size = in_seq.tellg();
    in_seq.seekg(0);
    seq.resize(prev_len + sequence_chunk_size);
    in_seq.read(&seq[prev_len], sequence_chunk_size);
    in_seq.close();
    remove(unpacked_seq_path.c_str());
  }

  bool done = false;
  uint32_t num_blocks_done = start_num / num_reads_per_block;
  uint32_t num_reads_done =
      num_blocks_done *
      num_reads_per_block; // denotes number of pairs done for PE
  while (!done) {
    uint32_t num_reads_cur_step =
        compute_num_reads_cur_step(num_reads, num_reads_done,
                                   num_reads_per_step, paired_end);
    if (num_reads_cur_step == 0)
      break;
    for (int stream_index = 0; stream_index < 2; stream_index++) {
      if (stream_index == 1 && !paired_end)
        continue;
#pragma omp parallel
      {
        uint64_t thread_id = omp_get_thread_num();
        if (thread_id * num_reads_per_block < num_reads_cur_step) {
          uint32_t thread_read_count =
              std::min((uint64_t)num_reads_cur_step,
                       (thread_id + 1) * num_reads_per_block) -
              thread_id * num_reads_per_block;

          if (stream_index == 0) {
            // Read decompression done when stream_index = 0 (even for PE)
            uint32_t block_num = num_blocks_done + thread_id;

            decompress_bsc_block(file_flag, block_num);
            decompress_bsc_block(file_pos, block_num);
            decompress_bsc_block(file_noise, block_num);
            decompress_bsc_block(file_noisepos, block_num);
            decompress_bsc_block(file_unaligned, block_num);
            decompress_bsc_block(file_readlength, block_num);
            decompress_bsc_block(file_RC, block_num);

            if (paired_end) {
              decompress_bsc_block(file_pos_pair, block_num);
              decompress_bsc_block(file_RC_pair, block_num);
            }

            // Read streams are shared between mates, so decode them once when
            // handling the first output stream.
            std::ifstream f_flag(block_file_path(file_flag, block_num));
            std::ifstream f_noise(block_file_path(file_noise, block_num));
            std::ifstream f_noisepos(block_file_path(file_noisepos, block_num),
                                     std::ios::binary);
            std::ifstream f_pos(block_file_path(file_pos, block_num),
                                std::ios::binary);
            std::ifstream f_RC(block_file_path(file_RC, block_num));
            std::ifstream f_unaligned(block_file_path(file_unaligned, block_num));
            std::ifstream f_readlength(block_file_path(file_readlength, block_num),
                                       std::ios::binary);
            std::ifstream f_pos_pair;
            std::ifstream f_RC_pair;
            if (paired_end) {
              f_pos_pair.open(block_file_path(file_pos_pair, block_num),
                              std::ios::binary);
              f_RC_pair.open(block_file_path(file_RC_pair, block_num));
            }

            char read_flag;
            uint64_t read_1_position;
            uint64_t read_2_position;
            uint64_t previous_position;
            bool read_1_is_singleton;
            bool read_2_is_singleton;
            char read_1_orientation;
            char read_2_orientation;
            uint16_t read_length;
            uint16_t position_delta_16;
            bool first_read_of_block = true;
            for (uint32_t i = thread_id * num_reads_per_block;
                 i < thread_id * num_reads_per_block + thread_read_count; i++) {
              f_flag >> read_flag;
              f_readlength.read(byte_ptr(&read_length), sizeof(uint16_t));
              read_lengths_buffer_1[i] = read_length;
              read_1_is_singleton =
                  (read_flag == '2') || (read_flag == '4');
              if (!read_1_is_singleton) {
                if (preserve_order)
                  f_pos.read(byte_ptr(&read_1_position), sizeof(uint64_t));
                else {
                  if (first_read_of_block) {
                    // Non-order-preserving mode stores the first absolute
                    // position in each block and then deltas afterward.
                    first_read_of_block = false;
                    f_pos.read(byte_ptr(&read_1_position), sizeof(uint64_t));
                    previous_position = read_1_position;
                  } else {
                    f_pos.read(byte_ptr(&position_delta_16), sizeof(uint16_t));
                    if (position_delta_16 == 65535)
                      f_pos.read(byte_ptr(&read_1_position),
                                 sizeof(uint64_t));
                    else
                      read_1_position = previous_position + position_delta_16;
                    previous_position = read_1_position;
                  }
                }
                f_RC >> read_1_orientation;
                std::string read =
                    seq.substr(read_1_position, read_lengths_buffer_1[i]);
                std::string noise_codes;
                uint16_t noise_position_delta;
                uint16_t previous_noise_position = 0;
                std::getline(f_noise, noise_codes);
                for (uint16_t k = 0; k < noise_codes.size(); k++) {
                  f_noisepos.read(byte_ptr(&noise_position_delta),
                                  sizeof(uint16_t));
                  noise_position_delta += previous_noise_position;
                  read[noise_position_delta] = decoded_noise_table
                      [(uint8_t)read[noise_position_delta]]
                      [(uint8_t)noise_codes[k]];
                  previous_noise_position = noise_position_delta;
                }
                if (read_1_orientation == 'd')
                  read_buffer_1[i] = read;
                else
                  read_buffer_1[i] =
                      reverse_complement(read, read_lengths_buffer_1[i]);
              } else {
                read_buffer_1[i].resize(read_lengths_buffer_1[i]);
                f_unaligned.read(&read_buffer_1[i][0], read_lengths_buffer_1[i]);
              }

              if (paired_end) {
                int16_t mate_position_delta_16;
                read_2_is_singleton =
                    (read_flag == '2') || (read_flag == '3');
                f_readlength.read(byte_ptr(&read_length), sizeof(uint16_t));
                read_lengths_buffer_2[i] = read_length;
                if (!read_2_is_singleton) {
                  if (read_flag == '1' || read_flag == '4') {
                    f_pos.read(byte_ptr(&read_2_position), sizeof(uint64_t));
                    f_RC >> read_2_orientation;
                  } else {
                    // Mate 2 can be stored relative to mate 1 inside the same
                    // block.
                    char relative_orientation_flag;
                    f_pos_pair.read(byte_ptr(&mate_position_delta_16),
                                    sizeof(int16_t));
                    read_2_position = read_1_position + mate_position_delta_16;
                    f_RC_pair >> relative_orientation_flag;
                    if (relative_orientation_flag == '0')
                      read_2_orientation =
                          (read_1_orientation == 'd') ? 'r' : 'd';
                    else
                      read_2_orientation =
                          (read_1_orientation == 'd') ? 'd' : 'r';
                  }
                  std::string read =
                      seq.substr(read_2_position, read_lengths_buffer_2[i]);
                  std::string noise_codes;
                  uint16_t noise_position_delta;
                  uint16_t previous_noise_position = 0;
                  std::getline(f_noise, noise_codes);
                  for (uint16_t k = 0; k < noise_codes.size(); k++) {
                    f_noisepos.read(byte_ptr(&noise_position_delta),
                                    sizeof(uint16_t));
                    noise_position_delta += previous_noise_position;
                    read[noise_position_delta] = decoded_noise_table
                        [(uint8_t)read[noise_position_delta]]
                        [(uint8_t)noise_codes[k]];
                    previous_noise_position = noise_position_delta;
                  }
                  if (read_2_orientation == 'd')
                    read_buffer_2[i] = read;
                  else
                    read_buffer_2[i] =
                        reverse_complement(read, read_lengths_buffer_2[i]);
                } else {
                  read_buffer_2[i].resize(read_lengths_buffer_2[i]);
                  f_unaligned.read(&read_buffer_2[i][0],
                                   read_lengths_buffer_2[i]);
                }
              }
            }

            f_flag.close();
            f_noise.close();
            f_noisepos.close();
            f_pos.close();
            f_RC.close();
            f_unaligned.close();
            f_readlength.close();
            if (paired_end) {
              f_pos_pair.close();
              f_RC_pair.close();
            }

            remove(block_file_path(file_flag, block_num).c_str());
            remove(block_file_path(file_pos, block_num).c_str());
            remove(block_file_path(file_noise, block_num).c_str());
            remove(block_file_path(file_noisepos, block_num).c_str());
            remove(block_file_path(file_unaligned, block_num).c_str());
            remove(block_file_path(file_readlength, block_num).c_str());
            remove(block_file_path(file_RC, block_num).c_str());
            if (paired_end) {
              remove(block_file_path(file_pos_pair, block_num).c_str());
              remove(block_file_path(file_RC_pair, block_num).c_str());
            }
          }
          // Decompress ids and quality
          uint32_t *read_lengths_buffer;
          std::string input_path;
          if (stream_index == 0)
            read_lengths_buffer = read_lengths_buffer_1;
          else
            read_lengths_buffer = read_lengths_buffer_2;
          if (preserve_quality) {
            input_path = input_quality_paths[stream_index] + "." +
                         std::to_string(num_blocks_done + thread_id);
            bsc::BSC_str_array_decompress(
                input_path.c_str(),
                quality_buffer + thread_id * num_reads_per_block,
                thread_read_count,
                read_lengths_buffer + thread_id * num_reads_per_block);
            remove(input_path.c_str());
          }
          if (!preserve_id) {
            for (uint32_t i = thread_id * num_reads_per_block;
                 i < thread_id * num_reads_per_block + thread_read_count; i++)
              id_buffer[i] = "@" + std::to_string(num_reads_done + i + 1) +
                             "/" + std::to_string(stream_index + 1);
          } else {
            if (stream_index == 1 && paired_id_match) {
              for (uint32_t i = thread_id * num_reads_per_block;
                   i < thread_id * num_reads_per_block + thread_read_count;
                   i++)
                modify_id(id_buffer[i], paired_id_code);
            } else {
              input_path = input_id_paths[stream_index] + "." +
                           std::to_string(num_blocks_done + thread_id);
              decompress_id_block(input_path.c_str(),
                                  id_buffer + thread_id * num_reads_per_block,
                                  thread_read_count);
              remove(input_path.c_str());
            }
          }
        }
      }

      std::string *read_buffer;
      if (stream_index == 0)
        read_buffer = read_buffer_1;
      else
        read_buffer = read_buffer_2;
      uint32_t num_reads_cur_step_output = num_reads_cur_step;
      if (num_reads_done + num_reads_cur_step_output >= end_num) {
        num_reads_cur_step_output = end_num - num_reads_done;
        done = true;
      }

      if (num_blocks_done == start_num / num_reads_per_block) {
        // The first emitted step can start partway through a stored block.
        uint32_t shift = start_num % num_reads_per_block;
        write_fastq_block(output_streams[stream_index], id_buffer + shift,
                          read_buffer + shift, quality_buffer + shift,
                          num_reads_cur_step_output - shift, preserve_quality,
                          num_thr, gzip_flag, gzip_level);
      } else {
        write_fastq_block(output_streams[stream_index], id_buffer, read_buffer,
                          quality_buffer,
                          num_reads_cur_step_output, preserve_quality, num_thr,
                          gzip_flag, gzip_level);
      }
    }
    num_reads_done += num_reads_cur_step;
    num_blocks_done += num_thr;
  }

  output_streams[0].close();
  if (paired_end)
    output_streams[1].close();

  delete[] read_buffer_1;
  if (paired_end)
    delete[] read_buffer_2;
  delete[] id_buffer;
  if (preserve_quality)
    delete[] quality_buffer;
  delete[] read_lengths_buffer_1;
  if (paired_end)
    delete[] read_lengths_buffer_2;
  for (int i = 0; i < 128; i++)
    delete[] decoded_noise_table[i];
  delete[] decoded_noise_table;
}

void decompress_long(const std::string &temp_dir, const std::string &outfile_1,
                     const std::string &outfile_2, const compression_params &cp,
                     const int &num_thr, const uint64_t &start_num,
                     const uint64_t &end_num, const bool &gzip_flag,
                     const int &gzip_level) {
  std::string input_read_paths[2];
  std::string input_quality_paths[2];
  std::string input_id_paths[2];
  std::string input_read_length_paths[2];
  std::string base_dir = temp_dir;
  input_read_paths[0] = base_dir + "/read_1";
  input_read_paths[1] = base_dir + "/read_2";
  input_quality_paths[0] = base_dir + "/quality_1";
  input_quality_paths[1] = base_dir + "/quality_2";
  input_id_paths[0] = base_dir + "/id_1";
  input_id_paths[1] = base_dir + "/id_2";
  input_read_length_paths[0] = base_dir + "/readlength_1";
  input_read_length_paths[1] = base_dir + "/readlength_2";

  uint32_t num_reads = cp.num_reads;
  uint8_t paired_id_code = cp.paired_id_code;
  bool paired_id_match = cp.paired_id_match;
  uint32_t num_reads_per_block = cp.num_reads_per_block_long;
  bool paired_end = cp.paired_end;
  bool preserve_id = cp.preserve_id;
  bool preserve_quality = cp.preserve_quality;

  std::string output_paths[2] = {outfile_1, outfile_2};
  std::ofstream output_streams[2];

  open_output_files(output_streams, output_paths, paired_end, gzip_flag);
  validate_output_files(output_streams, paired_end);

  const uint64_t num_reads_per_step =
      compute_num_reads_per_step(num_reads, num_reads_per_block, num_thr,
                                 paired_end);

  std::string *read_buffer = new std::string[num_reads_per_step];
  std::string *id_buffer = new std::string[num_reads_per_step];
  std::string *quality_buffer = NULL;
  if (preserve_quality)
    quality_buffer = new std::string[num_reads_per_step];
  uint32_t *read_lengths_buffer = new uint32_t[num_reads_per_step];

  omp_set_num_threads(num_thr);

  bool done = false;

  uint32_t num_blocks_done = start_num / num_reads_per_block;
  uint32_t num_reads_done =
      num_blocks_done *
      num_reads_per_block; // denotes number of pairs done for PE
  while (!done) {
    uint32_t num_reads_cur_step =
        compute_num_reads_cur_step(num_reads, num_reads_done,
                                   num_reads_per_step, paired_end);
    if (num_reads_cur_step == 0)
      break;
    for (int stream_index = 0; stream_index < 2; stream_index++) {
      if (stream_index == 1 && !paired_end)
        continue;
#pragma omp parallel
      {
        uint64_t thread_id = omp_get_thread_num();
        if (thread_id * num_reads_per_block < num_reads_cur_step) {
          uint32_t thread_read_count =
              std::min((uint64_t)num_reads_cur_step,
                       (thread_id + 1) * num_reads_per_block) -
              thread_id * num_reads_per_block;

          // Decompress read lengths file and read into array
          std::string compressed_read_length_path =
              input_read_length_paths[stream_index] + "." +
              std::to_string(num_blocks_done + thread_id) + ".bsc";
          std::string read_length_path =
              input_read_length_paths[stream_index] + "." +
              std::to_string(num_blocks_done + thread_id);
          bsc::BSC_decompress(compressed_read_length_path.c_str(),
                              read_length_path.c_str());
          remove(compressed_read_length_path.c_str());
          std::ifstream read_length_input(read_length_path, std::ios::binary);
          for (uint32_t i = thread_id * num_reads_per_block;
               i < thread_id * num_reads_per_block + thread_read_count; i++)
            read_length_input.read(byte_ptr(&read_lengths_buffer[i]),
                                sizeof(uint32_t));
          read_length_input.close();
          remove(read_length_path.c_str());

          std::string input_path = input_read_paths[stream_index] + "." +
                                   std::to_string(num_blocks_done + thread_id);
          bsc::BSC_str_array_decompress(
              input_path.c_str(), read_buffer + thread_id * num_reads_per_block,
              thread_read_count,
              read_lengths_buffer + thread_id * num_reads_per_block);
          remove(input_path.c_str());

          if (preserve_quality) {
            input_path = input_quality_paths[stream_index] + "." +
                         std::to_string(num_blocks_done + thread_id);
            bsc::BSC_str_array_decompress(
                input_path.c_str(),
                quality_buffer + thread_id * num_reads_per_block,
                thread_read_count,
                read_lengths_buffer + thread_id * num_reads_per_block);
            remove(input_path.c_str());
          }
          if (!preserve_id) {
            for (uint32_t i = thread_id * num_reads_per_block;
                 i < thread_id * num_reads_per_block + thread_read_count; i++)
              id_buffer[i] = "@" + std::to_string(num_reads_done + i + 1) +
                             "/" + std::to_string(stream_index + 1);
          } else {
            if (stream_index == 1 && paired_id_match) {
              for (uint32_t i = thread_id * num_reads_per_block;
                   i < thread_id * num_reads_per_block + thread_read_count;
                   i++)
                modify_id(id_buffer[i], paired_id_code);
            } else {
              input_path = input_id_paths[stream_index] + "." +
                           std::to_string(num_blocks_done + thread_id);
              decompress_id_block(input_path.c_str(),
                                  id_buffer + thread_id * num_reads_per_block,
                                  thread_read_count);
              remove(input_path.c_str());
            }
          }
        }
      }

      uint32_t num_reads_cur_step_output = num_reads_cur_step;
      if (num_reads_done + num_reads_cur_step_output >= end_num) {
        num_reads_cur_step_output = end_num - num_reads_done;
        done = true;
      }
      if (num_blocks_done == start_num / num_reads_per_block) {
        uint32_t shift = start_num % num_reads_per_block;
        write_fastq_block(output_streams[stream_index], id_buffer + shift,
                          read_buffer + shift, quality_buffer + shift,
                          num_reads_cur_step_output - shift, preserve_quality,
                          num_thr, gzip_flag, gzip_level);
      } else {
        write_fastq_block(output_streams[stream_index], id_buffer, read_buffer,
                          quality_buffer,
                          num_reads_cur_step_output, preserve_quality, num_thr,
                          gzip_flag, gzip_level);
      }
    }
    num_reads_done += num_reads_cur_step;
    num_blocks_done += num_thr;
  }

  output_streams[0].close();
  if (paired_end)
    output_streams[1].close();

  delete[] read_buffer;
  delete[] id_buffer;
  if (preserve_quality)
    delete[] quality_buffer;
  delete[] read_lengths_buffer;
}

void decompress_unpack_seq(const std::string &packed_seq_base_path,
               const int &encoding_thread_count,
               const int &decode_thread_count) {
#pragma omp parallel
  {
    int thread_id = omp_get_thread_num();
  for (int encoding_thread_id =
       thread_id * encoding_thread_count / decode_thread_count;
     encoding_thread_id <
     (thread_id + 1) * encoding_thread_count / decode_thread_count;
         encoding_thread_id++) {
      bsc::BSC_decompress(
      (packed_seq_base_path + '.' + std::to_string(encoding_thread_id) +
       ".bsc")
              .c_str(),
      (packed_seq_base_path + '.' + std::to_string(encoding_thread_id))
        .c_str());
      std::string infile_bsc =
      packed_seq_base_path + '.' + std::to_string(encoding_thread_id) +
      ".bsc";
      remove(infile_bsc.c_str());
    std::ofstream f_seq(packed_seq_base_path + '.' +
              std::to_string(encoding_thread_id) + ".tmp");
    std::ifstream in_seq(packed_seq_base_path + '.' +
                 std::to_string(encoding_thread_id),
               std::ios::binary);
    std::ifstream in_seq_tail(packed_seq_base_path + '.' +
                                std::to_string(encoding_thread_id) + ".tail");
      char int_to_base[4];
      int_to_base[0] = 'A';
      int_to_base[1] = 'C';
      int_to_base[2] = 'G';
      int_to_base[3] = 'T';

      uint8_t packed_base_byte;
      in_seq.read((char *)&packed_base_byte, sizeof(uint8_t));
      while (!in_seq.eof()) {
        f_seq << int_to_base[packed_base_byte % 4];
        packed_base_byte /= 4;
        f_seq << int_to_base[packed_base_byte % 4];
        packed_base_byte /= 4;
        f_seq << int_to_base[packed_base_byte % 4];
        packed_base_byte /= 4;
        f_seq << int_to_base[packed_base_byte % 4];
        in_seq.read((char *)&packed_base_byte, sizeof(uint8_t));
      }
      in_seq.close();

      f_seq << in_seq_tail.rdbuf();
      in_seq_tail.close();
      f_seq.close();

      remove((packed_seq_base_path + '.' + std::to_string(encoding_thread_id))
                 .c_str());
      remove((packed_seq_base_path + '.' +
              std::to_string(encoding_thread_id) + ".tail")
                 .c_str());
      rename((packed_seq_base_path + '.' +
              std::to_string(encoding_thread_id) + ".tmp")
                 .c_str(),
             (packed_seq_base_path + '.' + std::to_string(encoding_thread_id))
                 .c_str());
    }
  }
}

void set_dec_noise_array(char **dec_noise) {
  dec_noise[(uint8_t)'A'][(uint8_t)'0'] = 'C';
  dec_noise[(uint8_t)'A'][(uint8_t)'1'] = 'G';
  dec_noise[(uint8_t)'A'][(uint8_t)'2'] = 'T';
  dec_noise[(uint8_t)'A'][(uint8_t)'3'] = 'N';
  dec_noise[(uint8_t)'C'][(uint8_t)'0'] = 'A';
  dec_noise[(uint8_t)'C'][(uint8_t)'1'] = 'G';
  dec_noise[(uint8_t)'C'][(uint8_t)'2'] = 'T';
  dec_noise[(uint8_t)'C'][(uint8_t)'3'] = 'N';
  dec_noise[(uint8_t)'G'][(uint8_t)'0'] = 'T';
  dec_noise[(uint8_t)'G'][(uint8_t)'1'] = 'A';
  dec_noise[(uint8_t)'G'][(uint8_t)'2'] = 'C';
  dec_noise[(uint8_t)'G'][(uint8_t)'3'] = 'N';
  dec_noise[(uint8_t)'T'][(uint8_t)'0'] = 'G';
  dec_noise[(uint8_t)'T'][(uint8_t)'1'] = 'C';
  dec_noise[(uint8_t)'T'][(uint8_t)'2'] = 'A';
  dec_noise[(uint8_t)'T'][(uint8_t)'3'] = 'N';
  dec_noise[(uint8_t)'N'][(uint8_t)'0'] = 'A';
  dec_noise[(uint8_t)'N'][(uint8_t)'1'] = 'G';
  dec_noise[(uint8_t)'N'][(uint8_t)'2'] = 'C';
  dec_noise[(uint8_t)'N'][(uint8_t)'3'] = 'T';
}

} // namespace spring
