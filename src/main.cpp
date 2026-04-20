// Implements the Spring command-line entrypoint, including option parsing,
// temporary-directory management, and dispatch to compress/decompress modes.

#include "params.h"
#include "parse_utils.h"
#include "progress.h"
#include "spring.h"
#include "version.h"
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
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
  bool version_flag = false;
  bool compress_flag = false;
  bool decompress_flag = false;
  bool pairing_only_flag = false;
  bool no_quality_flag = false;
  bool no_ids_flag = false;
  bool audit_flag = false;
  std::string r1_path;
  std::string r2_path;
  std::vector<std::string> input_paths;
  std::vector<std::string> output_paths;
  std::vector<std::string> quality_options;
  std::string working_dir = ".";
  int num_threads = default_num_threads();
  bool num_threads_was_explicit = false;
  double memory_cap_gb = 0.0;
  int compression_level = spring::DEFAULT_COMPRESSION_LEVEL;
  std::string note;
  spring::log_level log_level = spring::log_level::quiet;
  bool unzip_flag = false;
};

class SpringContext {
public:
  // Disallow copying and moving to manage lifetime of temporary directories.
  SpringContext(const SpringContext &) = delete;
  SpringContext &operator=(const SpringContext &) = delete;
  SpringContext(SpringContext &&) = delete;
  SpringContext &operator=(SpringContext &&) = delete;

  explicit SpringContext(const std::string &working_dir) {
    const std::filesystem::path working_dir_path(working_dir);
    const bool working_dir_exists = std::filesystem::exists(working_dir_path);
    if (!working_dir_exists) {
      std::filesystem::create_directories(working_dir_path);
    }
    working_dir_ = working_dir_path;
    remove_working_dir_ = !working_dir_exists;

    temp_dir_ = create_temp_dir(working_dir_path);
    SPRING_LOG_INFO("Temporary directory: " +
                             temp_dir_.generic_string());
  }

  ~SpringContext() { cleanup(); }

  void cleanup() noexcept {
    if (!temp_dir_.empty()) {
      SPRING_LOG_INFO("Deleting temporary directory: " +
                               temp_dir_.generic_string());
      std::error_code ec;
      // Safety guard: only allow cleanup inside the configured working
      // directory. This prevents accidental recursive deletes if state gets
      // corrupted.
      std::filesystem::path canonical_working_dir =
          std::filesystem::weakly_canonical(working_dir_, ec);
      if (ec) {
        std::cerr << "Warning: Failed to canonicalize working directory '"
                  << working_dir_.generic_string() << "' during cleanup: "
                  << ec.message() << "\n";
        ec.clear();
      }

      std::filesystem::path canonical_temp_dir =
          std::filesystem::weakly_canonical(temp_dir_, ec);
      if (ec) {
        std::cerr << "Warning: Failed to canonicalize temporary directory '"
                  << temp_dir_.generic_string() << "' during cleanup: "
                  << ec.message() << "\n";
        ec.clear();
      }

      bool path_is_safe = true;
      if (!canonical_working_dir.empty() && !canonical_temp_dir.empty()) {
        const std::string working_prefix = canonical_working_dir.generic_string();
        const std::string temp_path = canonical_temp_dir.generic_string();
        path_is_safe =
            temp_path.size() >= working_prefix.size() &&
            temp_path.compare(0, working_prefix.size(), working_prefix) == 0;
      }

      if (!path_is_safe) {
        std::cerr << "Warning: Refusing to delete temporary directory outside "
                  << "working directory boundary: "
                  << temp_dir_.generic_string() << "\n";
      } else {
        const std::uintmax_t removed_count =
            std::filesystem::remove_all(temp_dir_, ec);
        if (ec) {
          std::cerr << "Warning: Failed to delete temporary directory '"
                    << temp_dir_.generic_string() << "': " << ec.message()
                    << "\n";
        } else {
          SPRING_LOG_DEBUG("Temporary cleanup removed " +
                           std::to_string(removed_count) + " filesystem "
                           "entries.");
        }
      }
      temp_dir_.clear();
    }
    if (remove_working_dir_ && !working_dir_.empty()) {
      std::error_code ec;
      if (std::filesystem::is_directory(working_dir_, ec) &&
          std::filesystem::is_empty(working_dir_, ec)) {
        SPRING_LOG_INFO("Deleting working directory...");
        std::filesystem::remove(working_dir_, ec);
      }
      remove_working_dir_ = false;
    }
  }

  [[nodiscard]] std::string temp_dir_path() const {
    return temp_dir_.generic_string() + '/';
  }

private:
  std::filesystem::path temp_dir_;
  std::filesystem::path working_dir_;
  bool remove_working_dir_ = false;

  static std::filesystem::path
  create_temp_dir(const std::filesystem::path &working_dir_path) {
    while (true) {
      const std::string random_str = "tmp." + spring::random_string(10);
      const std::filesystem::path temp_dir_path = working_dir_path / random_str;
      if (!std::filesystem::exists(temp_dir_path) &&
          std::filesystem::create_directory(temp_dir_path)) {
        return temp_dir_path;
      }
    }
  }
};

SpringContext *g_context = nullptr;

int print_invalid_mode_and_exit(const std::string &options_description) {
  std::cout << "Exactly one of compress or decompress needs to be specified \n";
  std::cout << options_description << "\n";
  return 1;
}

std::string build_options_description() {
  std::ostringstream options;
  options
      << "Allowed options:\n\n"
      << "* General Options:\n"
      << "  -h [ --help ]                   produce help message\n"
      << "  -V [ --version ]                produce version information\n"
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
      << "  -v [ --verbose ] [arg (=info)]  logging level: info or debug "
        "(default\n"
      << "                                  without -v: progress bar)\n"
      << "---------------------------------------------------------------------"
         "-----------\n"
      << "* Compression Options:\n"
      << "  -c [ --compress ]               compress\n"
      << "  -R1 [ --R1 ] arg                input read-1 file (required)\n"
      << "  -R2 [ --R2 ] arg                input read-2 file (optional; "
        "enables paired-end mode)\n"
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
      << "  -a [ --audit ]                  enable post-operation integrity "
         "verification\n"
      << "---------------------------------------------------------------------"
         "-----------\n"
      << "* Decompression Options:\n"
      << "  -d [ --decompress ]             decompress\n"
      << "  -i [ --input ] arg              input archive file (.sp)\n"
      << "  -u [ --unzip ]                  during decompression, force\n"
      << "                                  output to be uncompressed (even "
         "if\n"
      << "                                  original was .gz)";
  return options.str();
}

bool is_option_token(const std::string &token) {
  return !token.empty() && token[0] == '-';
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

spring::log_level parse_log_level(const std::string &value) {
  if (value == "info")
    return spring::log_level::info;
  if (value == "debug")
    return spring::log_level::debug;
  throw std::runtime_error("Invalid --verbose level: " + value +
                           ". Valid values are: info, debug.");
}

void parse_command_line(int argc, char **argv, command_line_options &options) {
  const std::vector<std::string> args(argv + 1, argv + argc);
  size_t index = 0;

  while (index < args.size()) {
    const std::string &arg = args[index++];

    if (arg == "-h" || arg == "--help") {
      options.help_flag = true;
    } else if (arg == "-V" || arg == "--version") {
      options.version_flag = true;
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
      options.num_threads = spring::parse_int_or_throw(
          args[index++], "Invalid number of threads.");
      options.num_threads_was_explicit = true;
    } else if (arg == "-m" || arg == "--memory") {
      require_value(args, index, "--memory");
      options.memory_cap_gb =
          spring::parse_double_or_throw(args[index++], "Invalid memory cap.");
    } else if (arg == "-l" || arg == "--level") {
      require_value(args, index, "--level");
      options.compression_level = spring::parse_int_or_throw(
          args[index++], "Invalid compression level.");
      if (options.compression_level < 1 || options.compression_level > 9)
        throw std::runtime_error("Compression level must be between 1 and 9.");
    } else if (arg == "-R1" || arg == "--R1") {
      require_value(args, index, "--R1");
      if (!options.r1_path.empty())
        throw std::runtime_error("--R1 can only be specified once.");
      options.r1_path = args[index++];
    } else if (arg == "-R2" || arg == "--R2") {
      require_value(args, index, "--R2");
      if (!options.r2_path.empty())
        throw std::runtime_error("--R2 can only be specified once.");
      options.r2_path = args[index++];
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
      options.log_level = spring::log_level::info;
      if (index < args.size() && !is_option_token(args[index])) {
        options.log_level = parse_log_level(args[index++]);
      }
    } else if (arg == "-a" || arg == "--audit") {
      options.audit_flag = true;
    } else if (arg == "-u" || arg == "--unzip") {
      options.unzip_flag = true;
    } else {
      throw std::runtime_error(std::string("Unknown option: ") + arg);
    }
  }

  if (options.decompress_flag && !options.compress_flag &&
      options.output_paths.size() > 2) {
    throw std::runtime_error("Decompression accepts at most 2 output files.");
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

void normalize_mode_specific_inputs(command_line_options &options) {
  if (options.compress_flag) {
    if (!options.input_paths.empty()) {
      throw std::runtime_error(
          "Compression no longer accepts --input. Use --R1 (required) and "
          "--R2 (optional).");
    }
    if (options.r1_path.empty()) {
      throw std::runtime_error(
          "Compression requires --R1 <file>. Optionally provide --R2 <file> "
          "for paired-end mode.");
    }
    options.input_paths.push_back(options.r1_path);
    if (!options.r2_path.empty()) {
      options.input_paths.push_back(options.r2_path);
    }
    return;
  }

  if (!options.r1_path.empty() || !options.r2_path.empty()) {
    throw std::runtime_error(
        "Decompression does not use --R1/--R2. Use --input <archive.sp>.");
  }
}

void validate_io_parameters(const command_line_options &options) {
  // Validate input files: must exist and be readable
  if (options.input_paths.empty()) {
    throw std::runtime_error("No input files specified.");
  }

  for (const auto &path : options.input_paths) {
    if (path.empty()) {
      throw std::runtime_error("Input path cannot be empty.");
    }
    if (!std::filesystem::exists(path)) {
      throw std::runtime_error("Input file does not exist: " + path);
    }
    if (!std::filesystem::is_regular_file(path)) {
      throw std::runtime_error("Input path is not a regular file: " + path);
    }
  }

  // Validate output paths: directories must exist or be creatable
  if (!options.output_paths.empty()) {
    for (const auto &path : options.output_paths) {
      if (path.empty()) {
        throw std::runtime_error("Output path cannot be empty.");
      }
      const std::filesystem::path output_path(path);
      const std::filesystem::path parent_dir = output_path.parent_path();
      if (!parent_dir.empty() && !std::filesystem::exists(parent_dir)) {
        throw std::runtime_error(
            "Output directory does not exist: " + parent_dir.generic_string());
      }
    }
  }

  // Validate compression input count: supports 1 (single-end) or 2 (paired-end) files
  if (options.compress_flag && 
      (options.input_paths.size() < 1 || options.input_paths.size() > 2)) {
    throw std::runtime_error(
        "Compression accepts 1 or 2 input files, but " +
        std::to_string(options.input_paths.size()) + " provided.");
  }

  if (options.decompress_flag && options.input_paths.size() != 1) {
    throw std::runtime_error(
        "Decompression requires exactly 1 input archive, but " +
        std::to_string(options.input_paths.size()) + " provided.");
  }

  // Validate decompression output count: at most 2 output files
  if (options.decompress_flag && options.output_paths.size() > 2) {
    throw std::runtime_error(
        "Decompression accepts at most 2 output files, but " +
        std::to_string(options.output_paths.size()) + " specified.");
  }
}

int max_threads_for_memory_cap_gb(const double memory_cap_gb) {
  if (memory_cap_gb <= 0.0)
    return 0;

  const int capped_threads = static_cast<int>(
      std::floor(memory_cap_gb / kApproxMemoryCapPerThreadGiB));
  return std::max(1, capped_threads);
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
  (void)options_description;
  std::cout << error_message << "\n";
  if (g_context)
    g_context->cleanup();
  return 1;
}

void run_requested_mode(const command_line_options &options,
                        const std::string &temp_dir) {
  if (options.compress_flag) {
    spring::compress(temp_dir, options.input_paths, options.output_paths,
                     options.num_threads, options.pairing_only_flag,
                     options.no_quality_flag, options.no_ids_flag,
                     options.quality_options, options.compression_level,
                     options.note, options.log_level, options.audit_flag);
    return;
  }

  spring::decompress(temp_dir, options.input_paths, options.output_paths,
                     options.num_threads, options.compression_level,
                     options.log_level, options.unzip_flag);
}

void log_options_for_debugging(const command_line_options &options) {
  if (!spring::Logger::is_debug_enabled())
    return;

  SPRING_LOG_DEBUG(
      "CLI mode: " +
      std::string(options.compress_flag ? "compress" : "decompress"));
  SPRING_LOG_DEBUG(
      "CLI settings: threads=" + std::to_string(options.num_threads) +
      ", memory_cap_gb=" + std::to_string(options.memory_cap_gb) +
      ", level=" + std::to_string(options.compression_level) +
      ", log_level=" +
      std::string(options.log_level == spring::log_level::debug ? "debug"
                                  : "info") +
      ", audit=" + std::string(options.audit_flag ? "true" : "false") +
      ", unzip=" + std::string(options.unzip_flag ? "true" : "false"));

  SPRING_LOG_DEBUG(
      "CLI strip flags: order=" +
      std::string(options.pairing_only_flag ? "true" : "false") +
      ", quality=" + std::string(options.no_quality_flag ? "true" : "false") +
      ", ids=" + std::string(options.no_ids_flag ? "true" : "false"));

  SPRING_LOG_DEBUG("CLI paths: inputs=" +
                            std::to_string(options.input_paths.size()) +
                            ", outputs=" +
                            std::to_string(options.output_paths.size()) +
                            ", tmp_dir=" + options.working_dir);
}

} // namespace

void signalHandler(int signum) {
  std::cout << "Interrupt signal (" << signum << ") received.\n";
  std::cout << "Program terminated unexpectedly\n";
  if (g_context) {
    g_context->cleanup();
  }
  std::exit(signum);
}

int main(int argc, char **argv) {
  signal(SIGINT, signalHandler);
  command_line_options options;
  const std::string options_description = build_options_description();

  try {
    parse_command_line(argc, argv, options);
    spring::Logger::set_level(options.log_level);
  } catch (const std::runtime_error &e) {
    std::cout << e.what() << "\n";
    std::cout << options_description << "\n";
    return 1;
  }

  if (options.help_flag) {
    std::cout << options_description << "\n";
    return 0;
  }
  if (options.version_flag) {
    std::cout << "spring2 version " << spring::VERSION << "\n";
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

  try {
    normalize_mode_specific_inputs(options);
  } catch (const std::runtime_error &e) {
    std::cout << e.what() << "\n";
    std::cout << options_description << "\n";
    return 1;
  }

  // Validate I/O parameters early: input files exist, output paths are valid.
  // This ensures any error during run_requested_mode is a true runtime error,
  // not a parameter issue.
  try {
    validate_io_parameters(options);
  } catch (const std::runtime_error &e) {
    std::cout << e.what() << "\n";
    std::cout << options_description << "\n";
    return 1;
  }

  // Isolate intermediate artifacts so cleanup is one directory removal.
  apply_memory_cap(options);
  log_options_for_debugging(options);
  SpringContext context(options.working_dir);
  g_context = &context;

  try {
    run_requested_mode(options, context.temp_dir_path());
  } catch (const std::runtime_error &e) {
    return print_unexpected_error_and_exit(
        options_description,
        std::string("Program terminated unexpectedly with error: ") + e.what());
  } catch (const std::exception &e) {
    return print_unexpected_error_and_exit(
        options_description,
        std::string("Program terminated unexpectedly with std::exception: ") +
            e.what());
  } catch (...) {
    return print_unexpected_error_and_exit(options_description,
                                           "Program terminated unexpectedly");
  }

  g_context = nullptr;
  return 0;
}

