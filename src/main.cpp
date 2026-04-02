// Implements the Spring command-line entrypoint, including option parsing,
// temporary-directory management, and dispatch to compress/decompress modes.

#include "spring.h"
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <system_error>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

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
std::string working_dir_global;
bool remove_working_dir_flag_global = false;

void delete_temp_dir_if_present() {
  if (!temp_dir_flag_global)
    return;

  std::cout << "Deleting temporary directory...\n";
  std::filesystem::remove_all(temp_dir_global);
  temp_dir_flag_global = false;
}

void delete_working_dir_if_present() {
  if (!remove_working_dir_flag_global)
    return;

  std::error_code error;
  if (std::filesystem::is_directory(working_dir_global, error) &&
      std::filesystem::is_empty(working_dir_global, error)) {
    std::cout << "Deleting working directory...\n";
    std::filesystem::remove(working_dir_global, error);
  }
  remove_working_dir_flag_global = false;
}

std::string create_temp_dir(const std::string &working_dir) {
  const std::filesystem::path working_dir_path(working_dir);
  while (true) {
    const std::string random_str = "tmp." + spring::random_string(10);
    const std::filesystem::path temp_dir_path = working_dir_path / random_str;
    if (!std::filesystem::exists(temp_dir_path) &&
        std::filesystem::create_directory(temp_dir_path)) {
      return temp_dir_path.string() + '/';
    }
  }
}

int print_invalid_mode_and_exit(
    const std::string &options_description) {
  std::cout
      << "Exactly one of compress or decompress needs to be specified \n";
  std::cout << options_description << "\n";
  return 1;
}

std::string build_options_description() {
  return R"(Allowed options:
  -h [ --help ]                   produce help message
  -c [ --compress ]               compress
  -d [ --decompress ]             decompress
  --decompress-range arg          --decompress-range start end
                                  (optional) decompress only reads (or read 
                                  pairs for PE datasets) from start to end 
                                  (both inclusive) (1 <= start <= end <= 
                                  num_reads (or num_read_pairs for PE)). If -r 
                                  was specified during compression, the range 
                                  of reads does not correspond to the original 
                                  order of reads in the FASTQ file.
  -i [ --input-file ] arg         input file name (two files for paired end)
  -o [ --output-file ] arg        output file name (for paired end 
                                  decompression, if only one file is specified,
                                  two output files will be created by suffixing
                                  .1 and .2.)
  -w [ --working-dir ] arg (=.)   directory to create temporary files (default 
                                  current directory)
  -t [ --num-threads ] arg (=8)   number of threads (default 8)
  -r [ --allow-read-reordering ]  do not retain read order during compression 
                                  (paired reads still remain paired)
  --no-quality                    do not retain quality values during 
                                  compression
  --no-ids                        do not retain read identifiers during 
                                  compression
  -q [ --quality-opts ] arg       quality mode: possible modes are
                                  1. -q lossless (default)
                                  2. -q qvz qv_ratio (QVZ lossy compression, 
                                  parameter qv_ratio roughly corresponds to 
                                  bits used per quality value)
                                  3. -q ill_bin (Illumina 8-level binning)
                                  4. -q binary thr high low (binary (2-level) 
                                  thresholding, quality binned to high if >= 
                                  thr and to low if < thr)
  -l [ --long ]                   Use for compression of arbitrarily long read 
                                  lengths. Can also provide better compression 
                                  for reads with significant number of indels. 
                                  -r disabled in this mode. For Illumina short 
                                  reads, compression is better without -l flag.
  --gzip-level arg (=6)           gzip level (0-9) to use when decompression 
                                  output path ends in .gz (default: 6)
  --fasta-input                   enable if compression input is fasta file 
                                  (i.e., no qualities))";
}

bool is_option_token(const std::string &token) {
  return !token.empty() && token[0] == '-';
}

int parse_int_or_throw(const std::string &value, const char *error_message) {
  try {
    size_t parsed_chars = 0;
    int parsed_value = std::stoi(value, &parsed_chars);
    if (parsed_chars != value.size())
      throw std::invalid_argument("trailing characters");
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
    if (parsed_chars != value.size())
      throw std::invalid_argument("trailing characters");
    return parsed_value;
  } catch (const std::exception &) {
    throw std::runtime_error(error_message);
  }
}

void require_value(const std::vector<std::string> &args, size_t index,
                   const char *option_name) {
  if (index >= args.size())
    throw std::runtime_error(std::string("Missing value for ") + option_name);
}

std::vector<std::string> collect_option_values(const std::vector<std::string> &args,
                                               size_t &index) {
  std::vector<std::string> values;
  while (index < args.size() && !is_option_token(args[index])) {
    values.push_back(args[index]);
    index++;
  }
  return values;
}

void parse_command_line(int argc, char **argv, command_line_options &options) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  size_t index = 0;

  while (index < args.size()) {
    const std::string &arg = args[index++];

    if (arg == "-h" || arg == "--help") {
      options.help_flag = true;
    } else if (arg == "-c" || arg == "--compress") {
      options.compress_flag = true;
    } else if (arg == "-d" || arg == "--decompress") {
      options.decompress_flag = true;
    } else if (arg == "-r" || arg == "--allow-read-reordering") {
      options.pairing_only_flag = true;
    } else if (arg == "--no-quality") {
      options.no_quality_flag = true;
    } else if (arg == "--no-ids") {
      options.no_ids_flag = true;
    } else if (arg == "-l" || arg == "--long") {
      options.long_flag = true;
    } else if (arg == "--fasta-input") {
      options.fasta_flag = true;
    } else if (arg == "-w" || arg == "--working-dir") {
      require_value(args, index, "--working-dir");
      options.working_dir = args[index++];
    } else if (arg == "-t" || arg == "--num-threads") {
      require_value(args, index, "--num-threads");
      options.num_threads = parse_int_or_throw(args[index++],
                                               "Invalid number of threads.");
    } else if (arg == "--gzip-level") {
      require_value(args, index, "--gzip-level");
      options.gzip_level = parse_int_or_throw(args[index++],
                                              "Invalid gzip level.");
    } else if (arg == "--decompress-range") {
      require_value(args, index, "--decompress-range");
      const std::vector<std::string> values = collect_option_values(args, index);
      if (values.size() != 2)
        throw std::runtime_error("--decompress-range requires exactly 2 values.");
      options.decompress_range = {
          parse_uint64_or_throw(values[0], "Invalid decompression range value."),
          parse_uint64_or_throw(values[1], "Invalid decompression range value.")};
    } else if (arg == "-i" || arg == "--input-file") {
      require_value(args, index, "--input-file");
      options.input_paths = collect_option_values(args, index);
      if (options.input_paths.empty())
        throw std::runtime_error("--input-file requires at least 1 value.");
    } else if (arg == "-o" || arg == "--output-file") {
      require_value(args, index, "--output-file");
      options.output_paths = collect_option_values(args, index);
      if (options.output_paths.empty())
        throw std::runtime_error("--output-file requires at least 1 value.");
    } else if (arg == "-q" || arg == "--quality-opts") {
      require_value(args, index, "--quality-opts");
      options.quality_options = collect_option_values(args, index);
      if (options.quality_options.empty())
        throw std::runtime_error("--quality-opts requires at least 1 value.");
    } else {
      throw std::runtime_error(std::string("Unknown option: ") + arg);
    }
  }
}

bool has_exactly_one_mode(const command_line_options &options) {
  return options.compress_flag != options.decompress_flag;
}

bool has_valid_thread_count(const command_line_options &options) {
  return options.num_threads > 0;
}

std::string create_and_register_temp_dir(const std::string &working_dir) {
  const bool working_dir_exists = std::filesystem::exists(working_dir);
  if (!working_dir_exists) {
    std::filesystem::create_directories(working_dir);
  }

  working_dir_global = working_dir;
  remove_working_dir_flag_global = !working_dir_exists;

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
  const std::string &options_description,
    const std::string &error_message) {
  std::cout << error_message << "\n";
  delete_temp_dir_if_present();
  delete_working_dir_if_present();
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
    std::filesystem::remove_all(temp_dir_global);
    temp_dir_flag_global = false;
  }
  delete_working_dir_if_present();
  std::exit(signum);
}

int main(int argc, char **argv) {
  signal(SIGINT, signalHandler);
  command_line_options options;
  const std::string options_description = build_options_description();

  try {
    parse_command_line(argc, argv, options);
  } catch (const std::runtime_error &e) {
    std::cout << e.what() << "\n";
    std::cout << options_description << "\n";
    return 1;
  }

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
  delete_working_dir_if_present();
  return 0;
}
