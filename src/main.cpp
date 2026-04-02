// Implements the Spring command-line entrypoint, including option parsing,
// temporary-directory management, and dispatch to compress/decompress modes.

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

namespace po = boost::program_options;

struct command_line_options {
  bool help_flag = false;
  bool compress_flag = false;
  bool decompress_flag = false;
  bool pairing_only_flag = false;
  bool no_quality_flag = false;
  bool no_ids_flag = false;
  bool long_flag = false;
  bool fasta_flag = false;
  std::vector<std::string> input_paths;
  std::vector<std::string> output_paths;
  std::vector<std::string> quality_options;
  std::vector<uint64_t> decompress_range;
  std::string working_dir = ".";
  int num_threads = 8;
  int gzip_level = 6;
};

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
    const po::options_description &options_description) {
  std::cout
      << "Exactly one of compress or decompress needs to be specified \n";
  std::cout << options_description << "\n";
  return 1;
}

po::options_description build_options_description(command_line_options &options) {
  po::options_description options_description("Allowed options");
  options_description.add_options()("help,h", po::bool_switch(&options.help_flag),
                                    "produce help message")(
      "compress,c", po::bool_switch(&options.compress_flag), "compress")(
      "decompress,d", po::bool_switch(&options.decompress_flag), "decompress")(
      "decompress-range",
      po::value<std::vector<uint64_t>>(&options.decompress_range)->multitoken(),
      "--decompress-range start end\n(optional) decompress only reads (or read "
      "pairs for PE datasets) from start to end (both inclusive) (1 <= start "
      "<= end <= num_reads (or num_read_pairs for PE)). If -r was specified "
      "during compression, the range of reads does not correspond to the "
      "original order of reads in the FASTQ file.")(
      "input-file,i",
      po::value<std::vector<std::string>>(&options.input_paths)->multitoken(),
      "input file name (two files for paired end)")(
      "output-file,o",
      po::value<std::vector<std::string>>(&options.output_paths)->multitoken(),
      "output file name (for paired end decompression, if only one file is "
      "specified, two output files will be created by suffixing .1 and .2.)")(
      "working-dir,w",
      po::value<std::string>(&options.working_dir)->default_value("."),
      "directory to create temporary files (default current directory)")(
      "num-threads,t", po::value<int>(&options.num_threads)->default_value(8),
      "number of threads (default 8)")(
      "allow-read-reordering,r", po::bool_switch(&options.pairing_only_flag),
      "do not retain read order during compression (paired reads still remain "
      "paired)")("no-quality", po::bool_switch(&options.no_quality_flag),
                   "do not retain quality values during compression")(
      "no-ids", po::bool_switch(&options.no_ids_flag),
      "do not retain read identifiers during compression")(
      "quality-opts,q",
      po::value<std::vector<std::string>>(&options.quality_options)->multitoken(),
      "quality mode: possible modes are\n1. -q lossless (default)\n2. -q qvz "
      "qv_ratio (QVZ lossy compression, parameter qv_ratio roughly corresponds "
      "to bits used per quality value)\n3. -q ill_bin (Illumina 8-level "
      "binning)\n4. -q binary thr high low (binary (2-level) thresholding, "
      "quality binned to high if >= thr and to low if < thr)")(
      "long,l", po::bool_switch(&options.long_flag),
      "Use for compression of arbitrarily long read lengths. Can also provide "
      "better compression for reads with significant number of indels. "
      "-r disabled in this mode. For Illumina short "
      "reads, compression is better without -l flag.")(
      "gzip-level", po::value<int>(&options.gzip_level)->default_value(6),
      "gzip level (0-9) to use when decompression output path ends in .gz "
      "(default: 6)")(
      "fasta-input", po::bool_switch(&options.fasta_flag),
      "enable if compression input is fasta file (i.e., no qualities)");
  return options_description;
}

void parse_command_line(int argc, char **argv,
                        const po::options_description &options_description) {
  po::variables_map variables_map;
  po::store(po::parse_command_line(argc, argv, options_description),
            variables_map);
  po::notify(variables_map);
}

bool has_exactly_one_mode(const command_line_options &options) {
  return options.compress_flag != options.decompress_flag;
}

bool has_valid_thread_count(const command_line_options &options) {
  return options.num_threads > 0;
}

std::string create_and_register_temp_dir(const std::string &working_dir) {
  const std::string temp_dir = create_temp_dir(working_dir);
  std::cout << "Temporary directory: " << temp_dir << "\n";
  temp_dir_global = temp_dir;
  temp_dir_flag_global = true;
  return temp_dir;
}

void normalize_compression_options(command_line_options &options) {
  if (!options.compress_flag || !options.long_flag)
    return;

  std::cout << "Long flag detected.\n";
  if (options.pairing_only_flag) {
    std::cout << "For long mode: allow_read_reordering flag is disabled.\n";
    options.pairing_only_flag = false;
  }
}

int print_unexpected_error_and_exit(
    const po::options_description &options_description,
    const std::string &error_message) {
  std::cout << error_message << "\n";
  delete_temp_dir_if_present();
  std::cout << options_description << "\n";
  return 1;
}

void run_requested_mode(const command_line_options &options,
                        const std::string &temp_dir) {
  if (options.compress_flag) {
    spring::compress(temp_dir, options.input_paths, options.output_paths,
                     options.num_threads, options.pairing_only_flag,
                     options.no_quality_flag, options.no_ids_flag,
                     options.quality_options, options.long_flag,
                     options.fasta_flag);
    return;
  }

  spring::decompress(temp_dir, options.input_paths, options.output_paths,
                     options.num_threads, options.decompress_range,
                     options.gzip_level);
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
  command_line_options options;
  po::options_description options_description =
      build_options_description(options);
  parse_command_line(argc, argv, options_description);

  if (options.help_flag) {
    std::cout << options_description << "\n";
    return 0;
  }

  if (!has_exactly_one_mode(options))
    return print_invalid_mode_and_exit(options_description);

  if (!has_valid_thread_count(options)) {
    std::cout << "Number of threads must be positive.\n";
    return 1;
  }

  // Isolate intermediate artifacts so cleanup is one directory removal.
  const std::string temp_dir = create_and_register_temp_dir(options.working_dir);
  normalize_compression_options(options);

  try {
    run_requested_mode(options, temp_dir);
  } catch (const std::runtime_error &e) {
    return print_unexpected_error_and_exit(
        options_description,
        std::string("Program terminated unexpectedly with error: ") +
            e.what());
  } catch (...) {
    return print_unexpected_error_and_exit(options_description,
                                           "Program terminated unexpectedly");
  }

  delete_temp_dir_if_present();
  return 0;
}
