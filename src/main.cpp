// Implements the Spring command-line entrypoint, including option parsing,
// temporary-directory management, and dispatch to compress/decompress modes.

#include "spring.h"
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <system_error>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr double kApproxMemoryCapPerThreadGiB = 1.0;

int default_num_threads() {
  const unsigned int hardware_threads = std::thread::hardware_concurrency();
  if (hardware_threads == 0)
    return 8;

  const int preferred_threads = static_cast<int>(hardware_threads) - 1;
  return std::min(std::max(1, preferred_threads), 16);
}

struct command_line_options {
  bool help_flag = false;
  bool compress_flag = false;
  bool decompress_flag = false;
  bool pairing_only_flag = false;
  bool no_quality_flag = false;
  bool no_ids_flag = false;
  bool long_flag = false;
  std::vector<std::string> input_paths;
  std::vector<std::string> output_paths;
  std::vector<std::string> quality_options;
  std::vector<uint64_t> decompress_range;
  std::string working_dir = ".";
  int num_threads = default_num_threads();
  bool num_threads_was_explicit = false;
  double memory_cap_gb = 0.0;
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
      return temp_dir_path.generic_string() + '/';
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
  std::ostringstream options;
  options << "Allowed options:\n"
    << "  -h [ --help ]                   produce help message\n"
    << "  -c [ --compress ]               compress\n"
    << "  -d [ --decompress ]             decompress\n"
    << "  --decompress-range arg          --decompress-range start end\n"
    << "                                  (optional) decompress only reads (or read\n"
    << "                                  pairs for PE datasets) from start to end\n"
    << "                                  (both inclusive) (1 <= start <= end <=\n"
    << "                                  num_reads (or num_read_pairs for PE)). If -r\n"
    << "                                  was specified during compression, the range\n"
    << "                                  of reads does not correspond to the original\n"
    << "                                  order of reads in the FASTQ file.\n"
    << "  -i [ --input-file ] arg         input file name (two files for paired end)\n"
    << "  -o [ --output-file ] arg        output file name (for paired end\n"
    << "                                  decompression, if only one file is specified,\n"
    << "                                  two output files will be created by suffixing\n"
    << "                                  .1 and .2.)\n"
    << "  -w [ --working-dir ] arg (=.)   directory to create temporary files (default\n"
    << "                                  current directory)\n"
    << "  -t [ --num-threads ] arg (=" << default_num_threads()
    << ")   number of threads\n"
    << "                                  (default: min(max(1, hw_threads - 1), 16))\n"
    << "  --memory-cap-gb arg (=0)       approximate memory budget in GB;\n"
    << "                                  reduces effective thread count using\n"
    << "                                  about 1 GB per worker thread (0 disables)\n"
    << "  -r [ --allow-read-reordering ]  do not retain read order during compression\n"
    << "                                  (paired reads still remain paired)\n"
    << "  --no-quality                    do not retain quality values during\n"
    << "                                  compression\n"
    << "  --no-ids                        do not retain read identifiers during\n"
    << "                                  compression\n"
    << "  -q [ --quality-opts ] arg       quality mode: possible modes are\n"
    << "                                  1. -q lossless (default)\n"
    << "                                  2. -q qvz qv_ratio (QVZ lossy compression,\n"
    << "                                  parameter qv_ratio roughly corresponds to\n"
    << "                                  bits used per quality value)\n"
    << "                                  3. -q ill_bin (Illumina 8-level binning)\n"
    << "                                  4. -q binary thr high low (binary (2-level)\n"
    << "                                  thresholding, quality binned to high if >=\n"
    << "                                  thr and to low if < thr)\n"
    << "  -l [ --long ]                   Use for compression of arbitrarily long read\n"
    << "                                  lengths. Can also provide better compression\n"
    << "                                  for reads with significant number of indels.\n"
    << "                                  -r disabled in this mode. For Illumina short\n"
    << "                                  reads, compression is better without -l flag.\n"
    << "  --gzip-level arg (=6)           gzip level (0-9) to use when decompression\n"
    << "                                  output path ends in .gz (default: 6)";
  return options.str();
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

double parse_double_or_throw(const std::string &value,
                             const char *error_message) {
  try {
    size_t parsed_chars = 0;
    double parsed_value = std::stod(value, &parsed_chars);
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
    } else if (arg == "-w" || arg == "--working-dir") {
      require_value(args, index, "--working-dir");
      options.working_dir = args[index++];
    } else if (arg == "-t" || arg == "--num-threads") {
      require_value(args, index, "--num-threads");
      options.num_threads = parse_int_or_throw(args[index++],
                                               "Invalid number of threads.");
      options.num_threads_was_explicit = true;
    } else if (arg == "--memory-cap-gb") {
      require_value(args, index, "--memory-cap-gb");
      options.memory_cap_gb = parse_double_or_throw(args[index++],
                                                    "Invalid memory cap.");
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

bool has_valid_memory_cap(const command_line_options &options) {
  return options.memory_cap_gb >= 0.0;
}

int max_threads_for_memory_cap_gb(const double memory_cap_gb) {
  if (memory_cap_gb <= 0.0)
    return 0;

  const int capped_threads =
      static_cast<int>(std::floor(memory_cap_gb / kApproxMemoryCapPerThreadGiB));
  return std::max(1, capped_threads);
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

void apply_memory_cap(command_line_options &options) {
  const int memory_capped_threads =
      max_threads_for_memory_cap_gb(options.memory_cap_gb);
  if (memory_capped_threads == 0 || options.num_threads <= memory_capped_threads)
    return;

  if (options.num_threads_was_explicit) {
    std::cout << "Memory cap detected; reducing requested thread count from "
              << options.num_threads << " to " << memory_capped_threads
              << ".\n";
  } else {
    std::cout << "Memory cap detected; reducing default thread count from "
              << options.num_threads << " to " << memory_capped_threads
              << ".\n";
  }
  options.num_threads = memory_capped_threads;
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
                     options.quality_options, options.long_flag);
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

  if (!has_valid_memory_cap(options)) {
    std::cout << "Memory cap must be non-negative.\n";
    return 1;
  }

  // Isolate intermediate artifacts so cleanup is one directory removal.
  normalize_compression_options(options);
  apply_memory_cap(options);
  const std::string temp_dir = create_and_register_temp_dir(options.working_dir);

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
