// Orchestrates top-level compression and decompression, including archive
// layout, temporary-file coordination, and user-facing workflow decisions.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip> // std::setw
#include <iostream>
#include <omp.h>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "decompress.h"
#include "paired_end_order.h"
#include "params.h"
#include "preprocess.h"
#include "progress.h"
#include "reordered_quality_id.h"
#include "reordered_streams.h"
#include "spring.h"
#include "template_dispatch.h"
#include "util.h"

namespace spring {

namespace {

using clock_type = std::chrono::steady_clock;

struct compression_io_config {
  std::string input_path_1;
  std::string input_path_2;
  std::string archive_path;
  bool paired_end;
};

struct decompression_io_config {
  std::string archive_path;
  std::string output_path_1;
  std::string output_path_2;
};

struct prepared_compression_inputs {
  std::string input_path_1;
  std::string input_path_2;
  bool input_1_was_gzipped;
  bool input_2_was_gzipped;
  bool input_1_actual_was_gzipped = false;
  bool input_2_actual_was_gzipped = false;
};

void decompress_gzip_input_file(const std::string &input_path,
                                const std::string &output_path,
                                int num_thr);

enum class input_record_format : uint8_t { fastq, fasta };

void print_step_summary(const char *step_name,
                        const clock_type::time_point &step_start,
                        const clock_type::time_point &step_end) {
  Logger::log_info(std::string(step_name) + " done!");
  Logger::log_info(
      "Time for this step: " +
      std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                         step_end - step_start)
                         .count()) +
      " s");
}

template <typename Func>
void run_timed_step(const char *start_message, const char *step_name,
                    Func &&step) {
  Logger::log_info(start_message);
  const auto step_start = clock_type::now();
  std::forward<Func>(step)();
  const auto step_end = clock_type::now();
  print_step_summary(step_name, step_start, step_end);
}

void run_system_command_or_throw(const std::string &command,
                                 const char *error_message) {
#ifdef _WIN32
  std::string wrapped_command = "\"" + command + "\"";
  const int command_status = std::system(wrapped_command.c_str());
#else
  const int command_status = std::system(command.c_str());
#endif
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

bool has_suffix(const std::string &value, const std::string &suffix) {
  if (suffix.size() > value.size())
    return false;
  return value.ends_with(suffix);
}

std::string to_ascii_lowercase(std::string value) {
  for (char &character : value) {
    character =
        static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  }
  return value;
}

bool is_gzip_input_path(const std::string &input_path) {
  return has_suffix(input_path, ".gz");
}

std::string strip_gzip_suffix(const std::string &input_path) {
  if (!is_gzip_input_path(input_path)) {
    return input_path;
  }
  return input_path.substr(0, input_path.size() - 3);
}

std::string decompressed_input_path(const std::string &temp_dir,
                                    const std::string &input_path,
                                    const int input_index) {
  const std::filesystem::path stripped_input_path(
      strip_gzip_suffix(input_path));
  const std::string filename = stripped_input_path.filename().string();
  return temp_dir + "/compression_input_" + std::to_string(input_index) + "." +
         filename;
}

#ifdef SPRING_RAPIDGZIP_EXECUTABLE
constexpr const char *kRapidgzipExecutable = SPRING_RAPIDGZIP_EXECUTABLE;
#else
constexpr const char *kRapidgzipExecutable = "";
#endif

std::string build_rapidgzip_command(const std::string &input_path,
                                    const std::string &output_path,
                                    const int num_thr) {
  const int decoder_parallelism = num_thr > 0 ? num_thr : 0;
  return shell_quote(shell_path(kRapidgzipExecutable)) +
         " --decompress --force --output " +
         shell_quote(shell_path(output_path)) + " --decoder-parallelism " +
         std::to_string(decoder_parallelism) + " " +
         shell_quote(shell_path(input_path));
}

void decompress_gzip_input_file_with_zlib(const std::string &input_path,
                                          const std::string &output_path) {
  std::ifstream fin(input_path, std::ios::binary);
  std::ofstream fout(output_path, std::ios::binary);
  if (!fin || !fout) {
    throw std::runtime_error("Failed to open files for staging decompression");
  }

  z_stream strm;
  std::memset(&strm, 0, sizeof(strm));
  if (inflateInit2(&strm, 15 + 16) != Z_OK) {
    throw std::runtime_error("Failed to initialize zlib");
  }

  std::vector<uint8_t> in_buf(1 << 16);
  std::vector<uint8_t> out_buf(1 << 16);

  while (true) {
    if (strm.avail_in == 0) {
      fin.read(reinterpret_cast<char *>(in_buf.data()), in_buf.size());
      strm.avail_in = static_cast<uInt>(fin.gcount());
      strm.next_in = in_buf.data();
      if (strm.avail_in == 0 && fin.eof()) {
        break;
      }
    }

    strm.avail_out = static_cast<uInt>(out_buf.size());
    strm.next_out = out_buf.data();

    int ret = inflate(&strm, Z_NO_FLUSH);

    if (strm.avail_out < out_buf.size()) {
      fout.write(reinterpret_cast<char *>(out_buf.data()),
                 out_buf.size() - strm.avail_out);
    }

    if (ret == Z_STREAM_END) {
      inflateReset(&strm);
    } else if (ret != Z_OK && ret != Z_BUF_ERROR) {
      // Data error or junk. Scan for next GZIP header (1F 8B 08)
      bool found = false;
      while (!found) {
        if (strm.avail_in >= 3) {
          if (strm.next_in[0] == 0x1f && strm.next_in[1] == 0x8b &&
              strm.next_in[2] == 0x08) {
            found = true;
          } else {
            strm.next_in++;
            strm.avail_in--;
          }
        } else {
          // Need more data to scan
          if (strm.avail_in > 0) {
            std::memmove(in_buf.data(), strm.next_in, strm.avail_in);
          }
          fin.read(reinterpret_cast<char *>(in_buf.data() + strm.avail_in),
                    in_buf.size() - strm.avail_in);
          size_t read = static_cast<size_t>(fin.gcount());
          if (read == 0 && fin.eof()) {
            return;
          }
          strm.avail_in += static_cast<uInt>(read);
          strm.next_in = in_buf.data();
        }
      }
      inflateReset(&strm);
    }
  }

  inflateEnd(&strm);
}

void decompress_gzip_input_file(const std::string &input_path,
                                const std::string &output_path,
                                const int num_thr) {
  if (kRapidgzipExecutable[0] != '\0' &&
      std::filesystem::exists(kRapidgzipExecutable)) {
    const std::string rapidgzip_command =
        build_rapidgzip_command(input_path, output_path, num_thr);
#ifdef _WIN32
    std::string wrapped_command = "\"" + rapidgzip_command + "\"";
    if (std::system(wrapped_command.c_str()) == 0) {
      return;
    }
#else
    if (std::system(rapidgzip_command.c_str()) == 0) {
      return;
    }
#endif
  }

  decompress_gzip_input_file_with_zlib(input_path, output_path);
}

prepared_compression_inputs
prepare_compression_inputs(const compression_io_config &io_config,
                           const std::string &temp_dir, const int num_thr) {
  prepared_compression_inputs prepared_inputs{
      .input_path_1 = io_config.input_path_1,
      .input_path_2 = io_config.input_path_2,
      .input_1_was_gzipped = false,
      .input_2_was_gzipped = false,
      .input_1_actual_was_gzipped = false,
      .input_2_actual_was_gzipped = false};

  const bool input_1_is_gzipped = is_gzip_input_path(io_config.input_path_1);
  if (input_1_is_gzipped) {
    prepared_inputs.input_path_1 =
        decompressed_input_path(temp_dir, io_config.input_path_1, 1);
    decompress_gzip_input_file(io_config.input_path_1,
                               prepared_inputs.input_path_1, num_thr);
    prepared_inputs.input_1_actual_was_gzipped = true;
  }

  if (io_config.paired_end) {
    const bool input_2_is_gzipped = is_gzip_input_path(io_config.input_path_2);
    if (input_2_is_gzipped) {
      prepared_inputs.input_path_2 =
          decompressed_input_path(temp_dir, io_config.input_path_2, 2);
      decompress_gzip_input_file(io_config.input_path_2,
                                 prepared_inputs.input_path_2, num_thr);
      prepared_inputs.input_2_actual_was_gzipped = true;
    }
  }

  return prepared_inputs;
}

void cleanup_prepared_compression_inputs(
    const prepared_compression_inputs &prepared_inputs, bool pairing_only_flag) {
  if (!pairing_only_flag) {
    if (prepared_inputs.input_1_actual_was_gzipped) {
      std::filesystem::remove(prepared_inputs.input_path_1);
    }
    if (prepared_inputs.input_2_actual_was_gzipped) {
      std::filesystem::remove(prepared_inputs.input_path_2);
    }
  }
}

bool is_fastq_extension(const std::string &path) {
  const std::string lowercase_path = to_ascii_lowercase(path);
  return has_suffix(lowercase_path, ".fastq") ||
         has_suffix(lowercase_path, ".fq");
}

bool is_fasta_extension(const std::string &path) {
  const std::string lowercase_path = to_ascii_lowercase(path);
  return has_suffix(lowercase_path, ".fasta") ||
         has_suffix(lowercase_path, ".fa") ||
         has_suffix(lowercase_path, ".fna");
}

input_record_format
detect_input_format_from_extension(const std::string &input_path,
                                   bool &detected) {
  if (is_fastq_extension(input_path)) {
    detected = true;
    return input_record_format::fastq;
  }
  if (is_fasta_extension(input_path)) {
    detected = true;
    return input_record_format::fasta;
  }

  detected = false;
  return input_record_format::fastq;
}

input_record_format
detect_input_format_from_content(const std::string &input_path,
                                 bool &detected) {
  std::ifstream input_stream(input_path);
  if (!input_stream.is_open()) {
    throw std::runtime_error("Error opening input file: " + input_path);
  }

  std::vector<std::string> non_empty_lines;
  std::string line;
  while (non_empty_lines.size() < 3 && std::getline(input_stream, line)) {
    remove_CR_from_end(line);
    if (!line.empty())
      non_empty_lines.push_back(line);
  }

  if (!non_empty_lines.empty()) {
    detected = true;
    const char first_marker = non_empty_lines[0][0];
    if (first_marker == '>')
      return input_record_format::fasta;
    if (first_marker != '@') {
      throw std::runtime_error(
          "Unable to detect whether input is FASTA or FASTQ: " + input_path);
    }

    if (non_empty_lines.size() < 3)
      return input_record_format::fasta;

    if (!non_empty_lines[2].empty() && non_empty_lines[2][0] == '+')
      return input_record_format::fastq;

    return input_record_format::fasta;
  }

  detected = false;
  return input_record_format::fastq;
}

const char *input_format_name(const input_record_format format) {
  return format == input_record_format::fasta ? "FASTA" : "FASTQ";
}

input_record_format detect_input_format(const std::string &input_path) {
  bool detected_from_extension = false;
  const input_record_format extension_format =
      detect_input_format_from_extension(input_path, detected_from_extension);
  if (detected_from_extension)
    return extension_format;

  bool detected_from_content = false;
  const input_record_format content_format =
      detect_input_format_from_content(input_path, detected_from_content);
  if (detected_from_content)
    return content_format;

  throw std::runtime_error("Unable to detect input format for empty file: " +
                           input_path);
}

compression_io_config resolve_compression_io(const string_list &input_paths,
                                             const string_list &output_paths) {
  compression_io_config io_config;

  switch (input_paths.size()) {
  case 0:
    throw std::runtime_error("No input file specified");
  case 1:
    io_config.paired_end = false;
    io_config.input_path_1 = input_paths[0];
    break;
  case 2:
    io_config.paired_end = true;
    io_config.input_path_1 = input_paths[0];
    io_config.input_path_2 = input_paths[1];
    break;
  default:
    throw std::runtime_error("Too many (>2) input files specified");
  }

  if (output_paths.empty()) {
    std::filesystem::path p = std::filesystem::path(input_paths[0]).filename();
    bool changed = true;
    while (changed) {
      changed = false;
      std::string ext = p.extension().string();
      for (const std::string &v : {".gz", ".fastq", ".fq", ".fasta", ".fa",
                                   ".FASTQ", ".FQ", ".FASTA", ".FA"}) {
        if (ext == v) {
          p.replace_extension("");
          changed = true;
          break;
        }
      }
    }
    p.replace_extension(".sp");
    io_config.archive_path = p.string();
  } else if (output_paths.size() == 1) {
    io_config.archive_path = output_paths[0];
  } else {
    throw std::runtime_error("Number of output files not equal to 1");
  }
  return io_config;
}

void configure_quality_options(compression_params &compression_params,
                               const string_list &quality_options) {
  if (quality_options.empty() || quality_options[0] == "lossless") {
    compression_params.qvz_flag = false;
    compression_params.ill_bin_flag = false;
    compression_params.bin_thr_flag = false;
    return;
  }

  if (quality_options[0] == "qvz") {
    if (quality_options.size() != 2)
      throw std::runtime_error("Invalid quality options.");

    compression_params.qvz_ratio = parse_double_or_throw(
        quality_options[1], "Invalid qvz ratio provided.");
    if (compression_params.qvz_ratio == 0.0)
      throw std::runtime_error("Invalid qvz ratio provided.");

    compression_params.qvz_flag = true;
    compression_params.ill_bin_flag = false;
    compression_params.bin_thr_flag = false;
    return;
  }

  if (quality_options[0] == "ill_bin") {
    compression_params.ill_bin_flag = true;
    compression_params.qvz_flag = false;
    compression_params.bin_thr_flag = false;
    return;
  }

  if (quality_options[0] == "binary") {
    if (quality_options.size() != 4)
      throw std::runtime_error("Invalid quality options.");

    compression_params.bin_thr_thr = parse_int_or_throw(
        quality_options[1], "Invalid binary quality threshold.");
    compression_params.bin_thr_high = parse_int_or_throw(
        quality_options[2], "Invalid binary high quality value.");
    compression_params.bin_thr_low = parse_int_or_throw(
        quality_options[3], "Invalid binary low quality value.");
    if (compression_params.bin_thr_thr > 94 ||
        compression_params.bin_thr_high > 94 ||
        compression_params.bin_thr_low > 94) {
      throw std::runtime_error(
          "Binary quality options must be in the range [0, 94].");
    }
    if (compression_params.bin_thr_high < compression_params.bin_thr_thr ||
        compression_params.bin_thr_low > compression_params.bin_thr_thr ||
        compression_params.bin_thr_high < compression_params.bin_thr_low) {
      throw std::runtime_error("Options do not satisfy low <= thr <= high.");
    }

    compression_params.qvz_flag = false;
    compression_params.ill_bin_flag = false;
    compression_params.bin_thr_flag = true;
    return;
  }

  throw std::runtime_error("Invalid quality options.");
}

void print_temp_dir_size(const std::string &temp_dir,
                         const char *label = "Temporary directory size") {
  Logger::log_info(std::string(label) + ": " +
                   std::to_string(get_directory_size(temp_dir)));
}

void print_compressed_stream_sizes(const std::string &temp_dir) {
  namespace fs = std::filesystem;

  uint64_t size_read = 0;
  uint64_t size_quality = 0;
  uint64_t size_id = 0;
  fs::path temp_dir_path{temp_dir};
  fs::directory_iterator entry_it{temp_dir_path};
  for (; entry_it != fs::directory_iterator{}; ++entry_it) {
    const std::string entry_name = entry_it->path().filename().string();
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

  Logger::log_info("");
  Logger::log_info("Sizes of streams after compression: ");
  Logger::log_info("Reads:      " + std::to_string(size_read) + " bytes");
  Logger::log_info("Quality:    " + std::to_string(size_quality) + " bytes");
  Logger::log_info("ID:         " + std::to_string(size_id) + " bytes");
}

decompression_io_config
resolve_decompression_io(const string_list &input_paths,
                         const string_list &output_paths,
                         const bool paired_end) {
  decompression_io_config io_config;

  if (input_paths.size() != 1)
    throw std::runtime_error("Number of input files not equal to 1");
  io_config.archive_path = input_paths[0];

  switch (output_paths.size()) {
  case 0:
    throw std::runtime_error("No output file specified");
  case 1:
    if (!paired_end) {
      io_config.output_path_1 = output_paths[0];
    } else {
      io_config.output_path_1 = output_paths[0] + ".1";
      io_config.output_path_2 = output_paths[0] + ".2";
    }
    break;
  case 2:
    if (!paired_end) {
      Logger::log_warning("Two output files provided for single end data. "
                          "Output will be written to the first file provided.");
      io_config.output_path_1 = output_paths[0];
    } else {
      io_config.output_path_1 = output_paths[0];
      io_config.output_path_2 = output_paths[1];
    }
    break;
  default:
    throw std::runtime_error("Too many (>2) output files specified");
  }

  return io_config;
}

} // namespace

void compress(const std::string &temp_dir,
              const std::vector<std::string> &input_paths,
              const std::vector<std::string> &output_paths, const int &num_thr,
              const bool &pairing_only_flag, const bool &no_quality_flag,
              const bool &no_ids_flag,
              const std::vector<std::string> &quality_options,
              const int &compression_level, const std::string &note,
              const bool verbose) {
  Logger::set_verbose(verbose);
  ProgressBar progress(!verbose);
  ProgressBar::SetGlobalInstance(&progress);
  omp_set_dynamic(0);

  Logger::log_info("Starting compression...");
  const auto compression_start = clock_type::now();

  const compression_io_config io_config =
      resolve_compression_io(input_paths, output_paths);
  const prepared_compression_inputs prepared_inputs =
      prepare_compression_inputs(io_config, temp_dir, num_thr);
  const input_record_format input_format_1 =
      detect_input_format(prepared_inputs.input_path_1);
  input_record_format input_format = input_format_1;
  if (io_config.paired_end) {
    const input_record_format input_format_2 =
        detect_input_format(prepared_inputs.input_path_2);
    if (input_format_1 != input_format_2) {
      cleanup_prepared_compression_inputs(prepared_inputs, pairing_only_flag);
      throw std::runtime_error(
          "Paired-end inputs must both be FASTQ or both be FASTA.");
    }
    input_format = input_format_2;
  }
  const bool fasta_input = input_format == input_record_format::fasta;
  const bool preserve_order = !pairing_only_flag;
  const bool preserve_id = !no_ids_flag;
  const bool preserve_quality = !no_quality_flag && !fasta_input;

  bool use_crlf = false;
  const uint32_t max_read_length = detect_max_read_length(
      prepared_inputs.input_path_1, prepared_inputs.input_path_2,
      io_config.paired_end, fasta_input, use_crlf);
  const bool long_flag = max_read_length > MAX_READ_LEN;

  if (long_flag) {
    Logger::log_info("Auto-detected long-read mode.");
  } else {
    Logger::log_info("Auto-detected short-read mode.");
  }

  compression_params cp{};
  cp.paired_end = io_config.paired_end;
  cp.preserve_order = preserve_order;
  cp.preserve_quality = preserve_quality;
  cp.preserve_id = preserve_id;
  cp.long_flag = long_flag;
  cp.use_crlf = use_crlf;
  cp.num_reads_per_block = NUM_READS_PER_BLOCK;
  cp.num_reads_per_block_long = NUM_READS_PER_BLOCK_LONG;
  cp.num_thr = num_thr;
  cp.compression_level = compression_level;
  cp.note = note;
  cp.fasta_mode = fasta_input;
  cp.input_filename_1 =
      std::filesystem::path(io_config.input_path_1).filename().string();
  if (io_config.paired_end) {
    cp.input_filename_2 =
        std::filesystem::path(io_config.input_path_2).filename().string();
  }

  // Extract detailed gzip metadata for input 1
  extract_gzip_detailed_info(
      io_config.input_path_1, cp.input_1_was_gzipped, cp.input_1_gzip_flg,
      cp.input_1_gzip_mtime, cp.input_1_gzip_xfl, cp.input_1_gzip_os,
      cp.input_1_gzip_name, cp.input_1_is_bgzf, cp.input_1_bgzf_block_size,
      cp.input_1_gzip_uncompressed_size, cp.input_1_gzip_compressed_size,
      cp.input_1_gzip_member_count);
  Logger::log_info("[GZIP-DIAG] R1 detection: was_gzipped=" + std::to_string(cp.input_1_was_gzipped) + " is_bgzf=" + std::to_string(cp.input_1_is_bgzf));

  // Extract detailed gzip metadata for input 2 (if paired-end)
  if (io_config.paired_end) {
    extract_gzip_detailed_info(
        io_config.input_path_2, cp.input_2_was_gzipped, cp.input_2_gzip_flg,
        cp.input_2_gzip_mtime, cp.input_2_gzip_xfl, cp.input_2_gzip_os,
        cp.input_2_gzip_name, cp.input_2_is_bgzf, cp.input_2_bgzf_block_size,
        cp.input_2_gzip_uncompressed_size, cp.input_2_gzip_compressed_size,
        cp.input_2_gzip_member_count);
    Logger::log_info("[GZIP-DIAG] R2 detection: was_gzipped=" + std::to_string(cp.input_2_was_gzipped) + " is_bgzf=" + std::to_string(cp.input_2_is_bgzf));
  }

  if (preserve_quality)
    configure_quality_options(cp, quality_options);

  Logger::log_info(std::string("Detected input format: ") +
                   input_format_name(input_format));
  if (fasta_input) {
    Logger::log_info(
        "FASTA input detected; quality values will not be stored.");
  }

  if (prepared_inputs.input_1_actual_was_gzipped ||
      prepared_inputs.input_2_actual_was_gzipped) {
    Logger::log_info("Detected gzipped input; decompressing to temporary input "
                     "files before compression.");
  }

  run_timed_step("Preprocessing ...", "Preprocessing", [&] {
    progress.set_stage("Preprocessing", 0.0F, 0.25F);
    preprocess(prepared_inputs.input_path_1, prepared_inputs.input_path_2,
               temp_dir, cp, fasta_input, &progress);
  });
  cleanup_prepared_compression_inputs(prepared_inputs, pairing_only_flag);
  print_temp_dir_size(temp_dir);

  if (!long_flag) {
    run_timed_step("Reordering ...", "Reordering", [&] {
      progress.set_stage("Reordering", 0.25F, 0.50F);
      call_reorder(temp_dir, cp);
    });

    print_temp_dir_size(temp_dir, "temp_dir size");

    run_timed_step("Encoding ...", "Encoding", [&] {
      progress.set_stage("Encoding", 0.50F, 0.85F);
      call_encoder(temp_dir, cp);
    });



    print_temp_dir_size(temp_dir);

    if (!preserve_order && (preserve_quality || preserve_id)) {
      run_timed_step("Reordering and compressing quality and/or ids ...",
                     "Reordering and compressing quality and/or ids",
                     [&] { reorder_compress_quality_id(temp_dir, cp); });
      print_temp_dir_size(temp_dir);
    }

    if (!preserve_order && io_config.paired_end) {
      run_timed_step("Encoding pairing information ...",
                     "Encoding pairing information",
                     [&] { pe_encode(temp_dir, cp); });
      print_temp_dir_size(temp_dir);
    }

    run_timed_step("Reordering and compressing streams ...",
                   "Reordering and compressing streams", [&] {
                     progress.set_stage("Compressing streams", 0.85F, 0.95F);
                     reorder_compress_streams(temp_dir, cp);
                   });
    print_temp_dir_size(temp_dir);
  }

  std::string compression_params_path = temp_dir + "/cp.bin";
  std::ofstream compression_params_output(compression_params_path,
                                          std::ios::binary);
  write_compression_params(compression_params_output, cp);
  compression_params_output.close();

  print_compressed_stream_sizes(temp_dir);

  run_timed_step("Creating tar archive ...", "Tar archive", [&] {
    progress.set_stage("Creating archive", 0.95F, 1.0F);
    const std::string tar_command =
        "tar -cf " + shell_quote(shell_path(io_config.archive_path)) + " -C " +
        shell_quote(shell_path(temp_dir)) + " .";
    run_system_command_or_throw(
        tar_command, "Error occurred during tar archive generation.");
  });

  const auto compression_end = clock_type::now();
  if (verbose) {
    std::cout << "Compression done!\n";
    std::cout << "Total time for compression: "
              << std::chrono::duration_cast<std::chrono::seconds>(
                     compression_end - compression_start)
                     .count()
              << " s\n";
  } else {
    progress.finalize();
  }

  if (verbose) {
    namespace fs = std::filesystem;
    fs::path archive_file_path{io_config.archive_path};
    std::cout << "\n";
    std::cout << "Total size: " << std::setw(12)
              << fs::file_size(archive_file_path) << " bytes\n";
  }
  ProgressBar::SetGlobalInstance(nullptr);
  return;
}

void decompress(const std::string &temp_dir,
                const std::vector<std::string> &input_paths,
                const std::vector<std::string> &output_paths,
                const int &num_thr, const int & /*compression_level*/,
                const bool verbose, const bool unzip_flag) {
  Logger::set_verbose(verbose);
  ProgressBar progress(!verbose);
  ProgressBar::SetGlobalInstance(&progress);
  omp_set_dynamic(0);

  Logger::log_info("Starting decompression...");
  const auto decompression_start = clock_type::now();
  compression_params cp{};

  if (input_paths.size() != 1)
    throw std::runtime_error("Number of input files not equal to 1");

  run_timed_step("Untarring tar archive ...", "Untarring archive", [&] {
    progress.set_stage("Untarring", 0.0F, 0.10F);
    const std::string untar_command =
        "tar -xf " + shell_quote(shell_path(input_paths[0])) + " -C " +
        shell_quote(shell_path(temp_dir));
    run_system_command_or_throw(untar_command,
                                "Error occurred during untarring.");
  });

  std::string compression_params_path = temp_dir + "/cp.bin";
  std::ifstream compression_params_input(compression_params_path,
                                         std::ios::binary);
  if (!compression_params_input.is_open())
    throw std::runtime_error("Can't open parameter file.");
  read_compression_params(compression_params_input, cp);
  if (!compression_params_input.good())
    throw std::runtime_error("Can't read compression parameters.");
  compression_params_input.close();

  if (verbose) {
    std::cout << "Original filenames detected in archive:\n";
    std::cout << "  Input 1: " << cp.input_filename_1 << "\n";
    if (cp.paired_end) {
      std::cout << "  Input 2: " << cp.input_filename_2 << "\n";
    }

    if (!cp.note.empty()) {
      std::cout << "Note: " << cp.note << "\n";
    }
  }

  auto has_compressed_suffix = [](const std::string &path) {
    return path.size() >= 3 && path.substr(path.size() - 3) == ".gz";
  };

  auto strip_compressed_suffix = [](const std::string &path) {
    if (path.size() >= 3 && path.substr(path.size() - 3) == ".gz")
      return path.substr(0, path.size() - 3);
    return path;
  };

  std::vector<std::string> resolved_output_paths = output_paths;
  if (resolved_output_paths.empty()) {
    resolved_output_paths.push_back(cp.input_filename_1);
    if (cp.paired_end) {
      resolved_output_paths.push_back(cp.input_filename_2);
    }
  }

  bool should_gzip[2] = {false, false};
  bool should_bgzf[2] = {false, false};

  auto determine_restoration = [&](int idx, std::string &path, bool was_gzipped,
                                   bool is_bgzf, uint8_t xfl) {
    bool has_suffix = has_compressed_suffix(path);
    if (unzip_flag) {
      if (was_gzipped || has_suffix) {
        path = strip_compressed_suffix(path);
      } else {
        std::cout << "Warning: Original input " << idx
                  << " was already not compressed. Ignoring --unzip.\n";
      }
      return;
    }

    if (has_suffix) {
      should_gzip[idx - 1] = true;
      should_bgzf[idx - 1] = is_bgzf;
      // Map XFL to compression level for restoration
      if (xfl == 2)
        cp.compression_level = 9;
      else if (xfl == 4)
        cp.compression_level = 1;
      // else keep default or user-provided level
    }
  };

  determine_restoration(1, resolved_output_paths[0], cp.input_1_was_gzipped,
                        cp.input_1_is_bgzf, cp.input_1_gzip_xfl);
  if (cp.paired_end) {
    std::string dummy_path2 = (resolved_output_paths.size() >= 2) ? resolved_output_paths[1] : resolved_output_paths[0];
    determine_restoration(2, dummy_path2, cp.input_2_was_gzipped,
                          cp.input_2_is_bgzf, cp.input_2_gzip_xfl);
    if (resolved_output_paths.size() >= 2) {
      resolved_output_paths[1] = dummy_path2;
    }
  }

  bool paired_end = cp.paired_end;
  const decompression_io_config io_config =
      resolve_decompression_io(input_paths, resolved_output_paths, paired_end);

  run_timed_step("Decompressing ...", "Decompressing", [&] {
    progress.set_stage("Decompressing", 0.10F, 1.0F);
    if (cp.long_flag) {
      decompress_long(temp_dir, io_config.output_path_1,
                      io_config.output_path_2, cp, cp.use_crlf, should_gzip,
                      should_bgzf);
    } else {
      decompress_short(temp_dir, io_config.output_path_1,
                       io_config.output_path_2, cp, cp.use_crlf, should_gzip,
                       should_bgzf);
    }
  });

  const auto decompression_end = clock_type::now();
  if (verbose) {
    std::cout << "Total time for decompression: "
              << std::chrono::duration_cast<std::chrono::seconds>(
                     decompression_end - decompression_start)
                     .count()
              << " s\n";
  } else {
    progress.finalize();
  }
  ProgressBar::SetGlobalInstance(nullptr);
}

std::string random_string(size_t length) {
  auto random_char = []() -> char {
    const char charset[] = "0123456789"
                           "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                           "abcdefghijklmnopqrstuvwxyz";
    const size_t max_index = (sizeof(charset) - 1);
    static thread_local std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<size_t> distribution(0, max_index - 1);
    return charset[distribution(generator)];
  };
  std::string random_value(length, 0);
  std::generate_n(random_value.begin(), length, random_char);
  return random_value;
}

} // namespace spring
