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

#include "spring.h"
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string temp_dir_global;
bool temp_dir_flag_global = false;

void delete_temp_dir_if_present() {
  if (!temp_dir_flag_global)
    return;

  std::cout << "Deleting temporary directory...\n";
  boost::filesystem::remove_all(temp_dir_global);
  temp_dir_flag_global = false;
}

std::string create_temp_dir(const std::string &working_dir) {
  while (true) {
    const std::string random_str = "tmp." + spring::random_string(10);
    const std::string temp_dir = working_dir + "/" + random_str + '/';
    if (!boost::filesystem::exists(temp_dir) &&
        boost::filesystem::create_directory(temp_dir)) {
      return temp_dir;
    }
  }
}

int print_invalid_mode_and_exit(
    const boost::program_options::options_description &options_description) {
  std::cout
      << "Exactly one of compress or decompress needs to be specified \n";
  std::cout << options_description << "\n";
  return 1;
}

void run_requested_mode(const bool compress_flag, const std::string &temp_dir,
                        const std::vector<std::string> &input_paths,
                        const std::vector<std::string> &output_paths,
                        const int num_thr, const bool pairing_only_flag,
                        const bool no_quality_flag, const bool no_ids_flag,
                        const std::vector<std::string> &quality_options,
                        const bool long_flag, const bool gzip_flag,
                        const bool fasta_flag,
                        const std::vector<uint64_t> &decompress_range,
                        const int gzip_level) {
  if (compress_flag) {
    spring::compress(temp_dir, input_paths, output_paths, num_thr,
                     pairing_only_flag, no_quality_flag, no_ids_flag,
                     quality_options, long_flag, gzip_flag, fasta_flag);
    return;
  }

  spring::decompress(temp_dir, input_paths, output_paths, num_thr,
                     decompress_range, gzip_flag, gzip_level);
}

} // namespace

void signalHandler(int signum) {
  std::cout << "Interrupt signal (" << signum << ") received.\n";
  std::cout << "Program terminated unexpectedly\n";
  if (temp_dir_flag_global) {
    std::cout << "Deleting temporary directory: " << temp_dir_global << "\n";
    boost::filesystem::remove_all(temp_dir_global);
    temp_dir_flag_global = false;
  }
  std::exit(signum);
}

int main(int argc, char **argv) {
  signal(SIGINT, signalHandler);
  namespace po = boost::program_options;
  bool help_flag = false;
  bool compress_flag = false;
  bool decompress_flag = false;
  bool pairing_only_flag = false;
  bool no_quality_flag = false;
  bool no_ids_flag = false;
  bool long_flag = false;
  bool gzip_flag = false;
  bool fasta_flag = false;
  std::vector<std::string> input_paths;
  std::vector<std::string> output_paths;
  std::vector<std::string> quality_options;
  std::vector<uint64_t> decompress_range;
  std::string working_dir;
  int num_thr, gzip_level;
  po::options_description options_description("Allowed options");
  options_description.add_options()("help,h", po::bool_switch(&help_flag),
                                    "produce help message")(
      "compress,c", po::bool_switch(&compress_flag), "compress")(
      "decompress,d", po::bool_switch(&decompress_flag), "decompress")(
      "decompress-range",
      po::value<std::vector<uint64_t>>(&decompress_range)->multitoken(),
      "--decompress-range start end\n(optional) decompress only reads (or read "
      "pairs for PE datasets) from start to end (both inclusive) (1 <= start "
      "<= end <= num_reads (or num_read_pairs for PE)). If -r was specified "
      "during compression, the range of reads does not correspond to the "
      "original order of reads in the FASTQ file.")(
      "input-file,i",
      po::value<std::vector<std::string>>(&input_paths)->multitoken(),
      "input file name (two files for paired end)")(
      "output-file,o",
      po::value<std::vector<std::string>>(&output_paths)->multitoken(),
      "output file name (for paired end decompression, if only one file is "
      "specified, two output files will be created by suffixing .1 and .2.)")(
      "working-dir,w", po::value<std::string>(&working_dir)->default_value("."),
      "directory to create temporary files (default current directory)")(
      "num-threads,t", po::value<int>(&num_thr)->default_value(8),
      "number of threads (default 8)")(
      "allow-read-reordering,r", po::bool_switch(&pairing_only_flag),
      "do not retain read order during compression (paired reads still remain "
      "paired)")("no-quality", po::bool_switch(&no_quality_flag),
                 "do not retain quality values during compression")(
      "no-ids", po::bool_switch(&no_ids_flag),
      "do not retain read identifiers during compression")(
      "quality-opts,q",
      po::value<std::vector<std::string>>(&quality_options)->multitoken(),
      "quality mode: possible modes are\n1. -q lossless (default)\n2. -q qvz "
      "qv_ratio (QVZ lossy compression, parameter qv_ratio roughly corresponds "
      "to bits used per quality value)\n3. -q ill_bin (Illumina 8-level "
      "binning)\n4. -q binary thr high low (binary (2-level) thresholding, "
      "quality binned to high if >= thr and to low if < thr)")(
      "long,l", po::bool_switch(&long_flag),
      "Use for compression of arbitrarily long read lengths. Can also provide "
      "better compression for reads with significant number of indels. "
      "-r disabled in this mode. For Illumina short "
      "reads, compression is better without -l flag.")(
      "gzipped-fastq,g", po::bool_switch(&gzip_flag),
      "enable if compression input is gzipped fastq or to output gzipped fastq "
      "during decompression")("gzip-level",
                              po::value<int>(&gzip_level)->default_value(6),
                              "gzip level (0-9) to use during decompression if "
                              "-g flag is specified (default: 6)")(
      "fasta-input", po::bool_switch(&fasta_flag),
      "enable if compression input is fasta file (i.e., no qualities)");
  po::variables_map vm;
  po::store(po::parse_command_line(argc, argv, options_description), vm);
  po::notify(vm);
  if (help_flag) {
    std::cout << options_description << "\n";
    return 0;
  }

  if ((!compress_flag && !decompress_flag) ||
      (compress_flag && decompress_flag))
    return print_invalid_mode_and_exit(options_description);

  if (num_thr <= 0) {
    std::cout << "Number of threads must be positive.\n";
    return 1;
  }

  // Isolate intermediate artifacts so cleanup is one directory removal.
  const std::string temp_dir = create_temp_dir(working_dir);
  std::cout << "Temporary directory: " << temp_dir << "\n";

  temp_dir_global = temp_dir;
  temp_dir_flag_global = true;

  if (compress_flag && long_flag) {
    std::cout << "Long flag detected.\n";
    if (pairing_only_flag) {
      std::cout << "For long mode: allow_read_reordering flag is disabled.\n";
      pairing_only_flag = false;
    }
  }
  try {
    run_requested_mode(compress_flag, temp_dir, input_paths, output_paths,
                       num_thr, pairing_only_flag, no_quality_flag,
                       no_ids_flag, quality_options, long_flag, gzip_flag,
                       fasta_flag, decompress_range, gzip_level);
  } catch (const std::runtime_error &e) {
    std::cout << "Program terminated unexpectedly with error: " << e.what()
              << "\n";
    delete_temp_dir_if_present();
    std::cout << options_description << "\n";
    return 1;
  } catch (...) {
    std::cout << "Program terminated unexpectedly\n";
    delete_temp_dir_if_present();
    std::cout << options_description << "\n";
    return 1;
  }

  delete_temp_dir_if_present();
  return 0;
}
