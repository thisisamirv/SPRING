// Implements the Spring command-line entrypoint, including option parsing,
// temporary-directory management, and dispatch to compress/decompress modes.

#include "params.h"
#include "progress.h"
#include "spring.h"
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
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
  std::vector<std::string> input_paths;
  std::vector<std::string> output_paths;
  std::vector<std::string> quality_options;
  std::string working_dir = ".";
  int num_threads = default_num_threads();
  bool num_threads_was_explicit = false;
  double memory_cap_gb = 0.0;
  int compression_level = spring::DEFAULT_COMPRESSION_LEVEL;
  std::string note;
  bool verbose_flag = false;
  bool unzip_flag = false;
};

std::string temp_dir_global;
bool temp_dir_flag_global = false;
std::string working_dir_global;
bool remove_working_dir_flag_global = false;

void delete_temp_dir_if_present() {
  if (!temp_dir_flag_global)
    return;

  spring::Logger::log_info(std::string("Deleting temporary directory: ") +
                           temp_dir_global);
  std::filesystem::path p(temp_dir_global);
  // remove_all can fail on Windows if path has trailing slash.
  // Converting to path object and using that is more robust.
  std::error_code ec;
  std::filesystem::remove_all(p, ec);
  if (ec) {
    std::cerr << "Warning: could not delete temporary directory: "
              << ec.message() << std::endl;
  }
  temp_dir_flag_global = false;
}

void delete_working_dir_if_present() {
  if (!remove_working_dir_flag_global)
    return;

  std::error_code error;
  if (std::filesystem::is_directory(working_dir_global, error) &&
      std::filesystem::is_empty(working_dir_global, error)) {
    spring::Logger::log_info("Deleting working directory...");
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

int print_invalid_mode_and_exit(const std::string &options_description) {
  std::cout << "Exactly one of compress or decompress needs to be specified \n";
  std::cout << options_description << "\n";
  return 1;
}

std::string build_options_description() {
  std::ostringstream options;
  options
      << "Allowed options:\n\n"
      << "General Options:\n"
      << "  -h [ --help ]                   produce help message\n"
      << "  -i [ --input ] arg              input file name (two files for "
         "paired end)\n"
      << "  -o [ --output ] arg             output file name\n"
      << "                                    - if not specified, it uses "
         "original input\n"
      << "                                      filenames (swapping extension "
         "to .sp during\n"
      << "                                      compression)\n"
      << "                                    - for paired end decompression, "
         "if only one file\n"
      << "                                      is specified, two output files "
         "will be created\n"
      << "                                      by suffixing .1 and .2\n"
      << "  -w [ --tmp-dir ] arg (=.)       directory to create temporary "
         "files (default\n"
      << "                                  current directory)\n"
      << "  -t [ --threads ] arg            number of threads (default:\n"
      << "                                  min(max(1, hw_threads - 1), 16))\n"
      << "  -m [ --memory ] arg (=0)        approximate memory budget in GB; "
         "reduces\n"
      << "                                  effective thread count using about "
         "1 GB per\n"
      << "                                  worker thread (0 disables)\n"
      << "  -v [ --verbose ]                enable extensive logging (default: "
         "progress bar)\n"
      << "--------------------------------------------------------------------------------\n"
      << "Compression Options:\n"
      << "  -c [ --compress ]               compress\n"
      << "  -l [ --level ] arg (=6)         compression level (1-9) to use for "
         "output\n"
      << "                                  (.gz) formatting (passed to gzip "
         "unchanged\n"
      << "                                  and scaled to Zstd 1-22 "
         "internally)\n"
      << "  -s [ --strip ] arg              discard data: i (ids), o (order), "
         "q (quality)\n"
      << "                                  Example: --strip io to drop ids "
         "and order.\n"
      << "  -q [ --qmod ] arg               quality mode: possible modes are\n"
      << "                                    1. -q lossless (default)\n"
      << "                                    2. -q qvz qv_ratio (QVZ lossy "
         "compression,\n"
      << "                                      parameter qv_ratio roughly "
         "corresponds to\n"
      << "                                      bits used per quality value)\n"
      << "                                    3. -q ill_bin (Illumina 8-level "
         "binning)\n"
      << "                                    4. -q binary thr high low "
         "(binary (2-level)\n"
      << "                                      thresholding, quality binned "
         "to high if >=\n"
      << "                                      thr and to low if < thr)\n"
      << "  -n [ --note ] arg               add a custom note to the archive\n"
      << "--------------------------------------------------------------------------------\n"
      << "Decompression Options:\n"
      << "  -d [ --decompress ]             decompress\n"
      << "  -u [ --unzip ]                  during decompression, force\n"
      << "                                  output to be uncompressed (even "
         "if\n"
      << "                                  original was .gz)";
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

std::string strip_quotes(const std::string &value) {
  if (value.size() >= 2) {
    if ((value.front() == '"' && value.back() == '"') ||
        (value.front() == '\'' && value.back() == '\'')) {
      return value.substr(1, value.size() - 2);
    }
  }
  return value;
}

void require_value(const std::vector<std::string> &args, size_t index,
                   const char *option_name) {
  if (index >= args.size())
    throw std::runtime_error(std::string("Missing value for ") + option_name);
}

std::vector<std::string>
collect_option_values(const std::vector<std::string> &args, size_t &index) {
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
    } else if (arg == "-s" || arg == "--strip") {
      require_value(args, index, "--strip");
      const std::string strip_options = args[index++];
      for (const char c : strip_options) {
        switch (c) {
        case 'i':
          options.no_ids_flag = true;
          break;
        case 'o':
          options.pairing_only_flag = true;
          break;
        case 'q':
          options.no_quality_flag = true;
          break;
        default:
          throw std::runtime_error("Invalid character '" + std::string(1, c) +
                                   "' in --strip. Valid are: i, o, q.");
        }
      }
    } else if (arg == "-w" || arg == "--tmp-dir") {
      require_value(args, index, "--tmp-dir");
      options.working_dir = args[index++];
    } else if (arg == "-t" || arg == "--threads") {
      require_value(args, index, "--threads");
      options.num_threads =
          parse_int_or_throw(args[index++], "Invalid number of threads.");
      options.num_threads_was_explicit = true;
    } else if (arg == "-m" || arg == "--memory") {
      require_value(args, index, "--memory");
      options.memory_cap_gb =
          parse_double_or_throw(args[index++], "Invalid memory cap.");
    } else if (arg == "-l" || arg == "--level") {
      require_value(args, index, "--level");
      options.compression_level =
          parse_int_or_throw(args[index++], "Invalid compression level.");
      if (options.compression_level < 1 || options.compression_level > 9)
        throw std::runtime_error("Compression level must be between 1 and 9.");
    } else if (arg == "-i" || arg == "--input") {
      require_value(args, index, "--input");
      const std::vector<std::string> values =
          collect_option_values(args, index);
      if (values.empty())
        throw std::runtime_error("--input requires at least 1 value.");
      options.input_paths.insert(options.input_paths.end(), values.begin(),
                                 values.end());
    } else if (arg == "-o" || arg == "--output") {
      require_value(args, index, "--output");
      const std::vector<std::string> values =
          collect_option_values(args, index);
      if (values.empty())
        throw std::runtime_error("--output requires at least 1 value.");
      options.output_paths.insert(options.output_paths.end(), values.begin(),
                                  values.end());
    } else if (arg == "-q" || arg == "--qmod") {
      require_value(args, index, "--qmod");
      options.quality_options = collect_option_values(args, index);
      if (options.quality_options.empty())
        throw std::runtime_error("--qmod requires at least 1 value.");
    } else if (arg == "-n" || arg == "--note") {
      require_value(args, index, "--note");
      options.note = strip_quotes(args[index++]);
    } else if (arg == "-v" || arg == "--verbose") {
      options.verbose_flag = true;
    } else if (arg == "-u" || arg == "--unzip") {
      options.unzip_flag = true;
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

  const int capped_threads = static_cast<int>(
      std::floor(memory_cap_gb / kApproxMemoryCapPerThreadGiB));
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
  spring::Logger::log_info("Temporary directory: " + temp_dir);
  temp_dir_global = temp_dir;
  temp_dir_flag_global = true;
  return temp_dir;
}

void apply_memory_cap(command_line_options &options) {
  const int memory_capped_threads =
      max_threads_for_memory_cap_gb(options.memory_cap_gb);
  if (memory_capped_threads == 0 ||
      options.num_threads <= memory_capped_threads)
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

int print_unexpected_error_and_exit(const std::string &options_description,
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
                     options.quality_options, options.compression_level,
                     options.note, options.verbose_flag);
    return;
  }

  spring::decompress(temp_dir, options.input_paths, options.output_paths,
                     options.num_threads, options.compression_level,
                     options.verbose_flag, options.unzip_flag);
}

} // namespace

void signalHandler(int signum) {
  std::cout << "Interrupt signal (" << signum << ") received.\n";
  std::cout << "Program terminated unexpectedly\n";
  if (temp_dir_flag_global) {
    spring::Logger::log_info(std::string("Deleting temporary directory: ") +
                             temp_dir_global);
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(temp_dir_global), ec);
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
    spring::Logger::set_verbose(options.verbose_flag);
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
  apply_memory_cap(options);
  const std::string temp_dir =
      create_and_register_temp_dir(options.working_dir);

  try {
    run_requested_mode(options, temp_dir);
  } catch (const std::runtime_error &e) {
    return print_unexpected_error_and_exit(
        options_description,
        std::string("Program terminated unexpectedly with error: ") + e.what());
  } catch (...) {
    return print_unexpected_error_and_exit(options_description,
                                           "Program terminated unexpectedly");
  }

  delete_temp_dir_if_present();
  delete_working_dir_if_present();
  return 0;
}
