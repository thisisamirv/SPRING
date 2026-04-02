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

#include <algorithm>
#include <boost/filesystem.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip> // std::setw
#include <iostream>
#include <omp.h>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "template_dispatch.h"
#include "decompress.h"
#include "params.h"
#include "paired_end_order.h"
#include "preprocess.h"
#include "reordered_quality_id.h"
#include "reordered_streams.h"
#include "spring.h"
#include "util.h"

namespace spring {

namespace {

using clock_type = std::chrono::steady_clock;

void print_step_summary(const char *step_name,
                        const clock_type::time_point &step_start,
                        const clock_type::time_point &step_end) {
  std::cout << step_name << " done!\n";
  std::cout << "Time for this step: "
            << std::chrono::duration_cast<std::chrono::seconds>(step_end -
                                                                step_start)
                   .count()
            << " s\n";
}

template <typename Func>
void run_timed_step(const char *start_message, const char *step_name,
                    Func &&step) {
  std::cout << start_message << "\n";
  const auto step_start = clock_type::now();
  std::forward<Func>(step)();
  const auto step_end = clock_type::now();
  print_step_summary(step_name, step_start, step_end);
}

void run_system_command_or_throw(const std::string &command,
                                 const char *error_message) {
  const int command_status = std::system(command.c_str());
  if (command_status != 0)
    throw std::runtime_error(error_message);
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

} // namespace

void compress(const std::string &temp_dir,
              const std::vector<std::string> &input_paths,
              const std::vector<std::string> &output_paths,
              const int &num_thr,
              const bool &pairing_only_flag, const bool &no_quality_flag,
              const bool &no_ids_flag,
              const std::vector<std::string> &quality_options,
              const bool &long_flag, const bool &gzip_flag,
              const bool &fasta_flag) {
  omp_set_dynamic(0);

  std::cout << "Starting compression...\n";
  const auto compression_start = clock_type::now();

  std::string input_path_1;
  std::string input_path_2;
  std::string archive_path;
  bool paired_end;
  bool preserve_quality;
  bool preserve_id;
  bool preserve_order;
  preserve_order = !pairing_only_flag;
  preserve_id = !no_ids_flag;
  preserve_quality = !no_quality_flag;
  if (fasta_flag)
    preserve_quality = false;
  switch (input_paths.size()) {
  case 0:
    throw std::runtime_error("No input file specified");
    break;
  case 1:
    paired_end = false;
    input_path_1 = input_paths[0];
    break;
  case 2:
    paired_end = true;
    input_path_1 = input_paths[0];
    input_path_2 = input_paths[1];
    break;
  default:
    throw std::runtime_error("Too many (>2) input files specified");
  }
  if (output_paths.size() == 1)
    archive_path = output_paths[0];
  else
    throw std::runtime_error("Number of output files not equal to 1");

  compression_params cp{};
  cp.paired_end = paired_end;
  cp.preserve_order = preserve_order;
  cp.preserve_id = preserve_id;
  cp.preserve_quality = preserve_quality;
  cp.long_flag = long_flag;
  cp.num_reads_per_block = NUM_READS_PER_BLOCK;
  cp.num_reads_per_block_long = NUM_READS_PER_BLOCK_LONG;
  cp.num_thr = num_thr;

  if (preserve_quality) {
    if (quality_options.empty()) {
      cp.qvz_flag = cp.ill_bin_flag = cp.bin_thr_flag = false;
    } else if (quality_options[0] == "lossless") {
      cp.qvz_flag = cp.ill_bin_flag = cp.bin_thr_flag = false;
    } else if (quality_options[0] == "qvz") {
      if (quality_options.size() != 2) {
        throw std::runtime_error("Invalid quality options.");
      } else {
        cp.qvz_ratio =
            parse_double_or_throw(quality_options[1],
                                  "Invalid qvz ratio provided.");
        if (cp.qvz_ratio == 0.0) {
          throw std::runtime_error("Invalid qvz ratio provided.");
        }
      }
      cp.qvz_flag = true;
      cp.ill_bin_flag = cp.bin_thr_flag = false;
    } else if (quality_options[0] == "ill_bin") {
      cp.ill_bin_flag = true;
      cp.qvz_flag = cp.bin_thr_flag = false;
    } else if (quality_options[0] == "binary") {
      if (quality_options.size() != 4) {
        throw std::runtime_error("Invalid quality options.");
      } else {
        cp.bin_thr_thr = parse_int_or_throw(quality_options[1],
                                            "Invalid binary quality threshold.");
        cp.bin_thr_high = parse_int_or_throw(quality_options[2],
                                             "Invalid binary high quality value.");
        cp.bin_thr_low = parse_int_or_throw(quality_options[3],
                                            "Invalid binary low quality value.");
        if (cp.bin_thr_thr > 94 || cp.bin_thr_high > 94 ||
          cp.bin_thr_low > 94) {
          throw std::runtime_error(
            "Binary quality options must be in the range [0, 94].");
        }
        if (cp.bin_thr_high < cp.bin_thr_thr ||
            cp.bin_thr_low > cp.bin_thr_thr ||
            cp.bin_thr_high < cp.bin_thr_low) {
          throw std::runtime_error(
              "Options do not satisfy low <= thr <= high.");
        }
      }
      cp.qvz_flag = cp.ill_bin_flag = false;
      cp.bin_thr_flag = true;
    } else {
      throw std::runtime_error("Invalid quality options.");
    }
  }

  run_timed_step("Preprocessing ...", "Preprocessing", [&] {
    preprocess(input_path_1, input_path_2, temp_dir, cp, gzip_flag,
               fasta_flag);
  });
  std::cout << "Temporary directory size: " << get_directory_size(temp_dir)
            << "\n";

  if (!long_flag) {
    run_timed_step("Reordering ...", "Reordering",
                   [&] { call_reorder(temp_dir, cp); });

    std::cout << "temp_dir size: " << get_directory_size(temp_dir) << "\n";

    run_timed_step("Encoding ...", "Encoding",
                   [&] { call_encoder(temp_dir, cp); });
    std::cout << "Temporary directory size: " << get_directory_size(temp_dir)
              << "\n";

    if (!preserve_order && (preserve_quality || preserve_id)) {
      run_timed_step("Reordering and compressing quality and/or ids ...",
                     "Reordering and compressing quality and/or ids", [&] {
                       reorder_compress_quality_id(temp_dir, cp);
                     });
      std::cout << "Temporary directory size: " << get_directory_size(temp_dir)
                << "\n";
    }

    if (!preserve_order && paired_end) {
      run_timed_step("Encoding pairing information ...",
                     "Encoding pairing information",
                     [&] { pe_encode(temp_dir, cp); });
      std::cout << "Temporary directory size: " << get_directory_size(temp_dir)
                << "\n";
    }

    run_timed_step("Reordering and compressing streams ...",
                   "Reordering and compressing streams",
                   [&] { reorder_compress_streams(temp_dir, cp); });
    std::cout << "Temporary directory size: " << get_directory_size(temp_dir)
              << "\n";
  }

  std::string compression_params_path = temp_dir + "/cp.bin";
  std::ofstream compression_params_output(compression_params_path,
                                          std::ios::binary);
  compression_params_output.write(byte_ptr(&cp), sizeof(compression_params));
  compression_params_output.close();

  namespace fs = boost::filesystem;
  uint64_t size_read = 0;
  uint64_t size_quality = 0;
  uint64_t size_id = 0;
  fs::path temp_dir_path{temp_dir};
  fs::directory_iterator entry_it{temp_dir_path};
  for (; entry_it != fs::directory_iterator{}; ++entry_it) {
    std::string entry_name = entry_it->path().filename().string();
    switch (entry_name[0]) {
    case 'r':
      size_read += fs::file_size(entry_it->path());
      break;
    case 'q':
      size_quality += fs::file_size(entry_it->path());
      break;
    case 'i':
      size_id += fs::file_size(entry_it->path());
      break;
    }
  }
  std::cout << "\n";
  std::cout << "Sizes of streams after compression: \n";
  std::cout << "Reads:      " << std::setw(12) << size_read << " bytes\n";
  std::cout << "Quality:    " << std::setw(12) << size_quality << " bytes\n";
  std::cout << "ID:         " << std::setw(12) << size_id << " bytes\n";

  run_timed_step("Creating tar archive ...", "Tar archive", [&] {
    const std::string tar_command =
        "tar -cf " + archive_path + " -C " + temp_dir + " . ";
    run_system_command_or_throw(tar_command,
                                "Error occurred during tar archive generation.");
  });

  const auto compression_end = clock_type::now();
  std::cout << "Compression done!\n";
  std::cout << "Total time for compression: "
            << std::chrono::duration_cast<std::chrono::seconds>(
                   compression_end - compression_start)
                   .count()
            << " s\n";

  fs::path archive_file_path{archive_path};
  std::cout << "\n";
  std::cout << "Total size: " << std::setw(12)
            << fs::file_size(archive_file_path)
            << " bytes\n";
  return;
}

void decompress(const std::string &temp_dir,
                const std::vector<std::string> &input_paths,
                const std::vector<std::string> &output_paths,
                const int &num_thr,
                const std::vector<uint64_t> &decompress_range,
                const bool &gzip_flag, const int &gzip_level) {
  omp_set_dynamic(0);

  std::cout << "Starting decompression...\n";
  const auto decompression_start = clock_type::now();
  compression_params cp{};

  std::string archive_path;
  std::string output_path_1;
  std::string output_path_2;

  if (input_paths.size() == 1)
    archive_path = input_paths[0];
  else
    throw std::runtime_error("Number of input files not equal to 1");

  run_timed_step("Untarring tar archive ...", "Untarring archive", [&] {
    const std::string untar_command =
        "tar -xf " + archive_path + " -C " + temp_dir;
    run_system_command_or_throw(untar_command,
                                "Error occurred during untarring.");
  });

  std::string compression_params_path = temp_dir + "/cp.bin";
  std::ifstream compression_params_input(compression_params_path,
                                         std::ios::binary);
  if (!compression_params_input.is_open())
    throw std::runtime_error("Can't open parameter file.");
  compression_params_input.read(byte_ptr(&cp), sizeof(compression_params));
  if (!compression_params_input.good())
    throw std::runtime_error("Can't read compression parameters.");
  compression_params_input.close();

  bool paired_end = cp.paired_end;
  bool long_flag = cp.long_flag;

  switch (output_paths.size()) {
  case 0:
    throw std::runtime_error("No output file specified");
    break;
  case 1:
    if (!paired_end)
      output_path_1 = output_paths[0];
    if (paired_end) {
      output_path_1 = output_paths[0] + ".1";
      output_path_2 = output_paths[0] + ".2";
    }
    break;
  case 2:
    if (!paired_end) {
      std::cerr << "WARNING: Two output files provided for single end data. "
                   "Output will be written to the first file provided.";
      output_path_1 = output_paths[0];
    } else {
      output_path_1 = output_paths[0];
      output_path_2 = output_paths[1];
    }
    break;
  default:
    throw std::runtime_error("Too many (>2) output files specified");
  }
  uint64_t num_read_pairs = paired_end ? cp.num_reads / 2 : cp.num_reads;
  uint64_t start_num = 0;
  uint64_t end_num = num_read_pairs;
  if (decompress_range.size() != 0) {
    if (decompress_range.size() != 2)
      throw std::runtime_error("Invalid decompression range parameters.");
    if (decompress_range[0] == 0 ||
        decompress_range[0] > decompress_range[1] ||
        decompress_range[0] > num_read_pairs ||
        decompress_range[1] > num_read_pairs)
      throw std::runtime_error("Invalid decompression range parameters.");
    start_num = decompress_range[0] - 1;
    end_num = decompress_range[1];
  }

  // Long-read and short-read archives diverge only at the reconstruction step.
  run_timed_step("Decompressing ...", "Decompressing", [&] {
    if (long_flag)
      decompress_long(temp_dir, output_path_1, output_path_2, cp, num_thr,
                      start_num, end_num, gzip_flag, gzip_level);
    else
      decompress_short(temp_dir, output_path_1, output_path_2, cp, num_thr,
                       start_num, end_num, gzip_flag, gzip_level);
  });

  const auto decompression_end = clock_type::now();
  std::cout << "Total time for decompression: "
            << std::chrono::duration_cast<std::chrono::seconds>(
                   decompression_end - decompression_start)
                   .count()
            << " s\n";
}

std::string random_string(size_t length) {
  auto random_char = []() -> char {
    const char charset[] = "0123456789"
                           "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                           "abcdefghijklmnopqrstuvwxyz";
    const size_t max_index = (sizeof(charset) - 1);
    return charset[rand() % max_index];
  };
  std::string random_value(length, 0);
  std::generate_n(random_value.begin(), length, random_char);
  return random_value;
}

} // namespace spring
