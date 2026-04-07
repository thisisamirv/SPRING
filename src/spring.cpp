// Orchestrates top-level compression and decompression, including archive
// layout, temporary-file coordination, and user-facing workflow decisions.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
#include "reordered_quality_id.h"
#include "reordered_streams.h"
#include "spring.h"
#include "template_dispatch.h"
#include "util.h"

namespace spring {

void write_bool(std::ostream &out, bool value) {
  uint8_t byte = value ? 1 : 0;
  out.write(byte_ptr(&byte), sizeof(uint8_t));
}

bool read_bool(std::istream &in) {
  uint8_t byte = 0;
  in.read(byte_ptr(&byte), sizeof(uint8_t));
  return byte != 0;
}

void write_compression_params(std::ostream &out, const compression_params &cp) {
  write_bool(out, cp.paired_end);
  write_bool(out, cp.preserve_order);
  write_bool(out, cp.preserve_quality);
  write_bool(out, cp.preserve_id);
  write_bool(out, cp.long_flag);
  write_bool(out, cp.qvz_flag);
  write_bool(out, cp.ill_bin_flag);
  write_bool(out, cp.bin_thr_flag);
  out.write(byte_ptr(&cp.qvz_ratio), sizeof(double));
  out.write(byte_ptr(&cp.bin_thr_thr), sizeof(unsigned int));
  out.write(byte_ptr(&cp.bin_thr_high), sizeof(unsigned int));
  out.write(byte_ptr(&cp.bin_thr_low), sizeof(unsigned int));
  out.write(byte_ptr(&cp.num_reads), sizeof(uint32_t));
  out.write(byte_ptr(&cp.num_reads_clean[0]), sizeof(uint32_t));
  out.write(byte_ptr(&cp.num_reads_clean[1]), sizeof(uint32_t));
  out.write(byte_ptr(&cp.max_readlen), sizeof(uint32_t));
  out.write(byte_ptr(&cp.paired_id_code), sizeof(uint8_t));
  write_bool(out, cp.paired_id_match);
  out.write(byte_ptr(&cp.num_reads_per_block), sizeof(int));
  out.write(byte_ptr(&cp.num_reads_per_block_long), sizeof(int));
  out.write(byte_ptr(&cp.num_thr), sizeof(int));
  out.write(byte_ptr(&cp.compression_level), sizeof(int));
  out.write(reinterpret_cast<const char *>(cp.file_len_seq_thr),
            sizeof(uint64_t) * compression_params::kFileLenThrSize);
  out.write(reinterpret_cast<const char *>(cp.file_len_id_thr),
            sizeof(uint64_t) * compression_params::kFileLenThrSize);
}

void read_compression_params(std::istream &in, compression_params &cp) {
  cp.paired_end = read_bool(in);
  cp.preserve_order = read_bool(in);
  cp.preserve_quality = read_bool(in);
  cp.preserve_id = read_bool(in);
  cp.long_flag = read_bool(in);
  cp.qvz_flag = read_bool(in);
  cp.ill_bin_flag = read_bool(in);
  cp.bin_thr_flag = read_bool(in);
  in.read(byte_ptr(&cp.qvz_ratio), sizeof(double));
  in.read(byte_ptr(&cp.bin_thr_thr), sizeof(unsigned int));
  in.read(byte_ptr(&cp.bin_thr_high), sizeof(unsigned int));
  in.read(byte_ptr(&cp.bin_thr_low), sizeof(unsigned int));
  in.read(byte_ptr(&cp.num_reads), sizeof(uint32_t));
  in.read(byte_ptr(&cp.num_reads_clean[0]), sizeof(uint32_t));
  in.read(byte_ptr(&cp.num_reads_clean[1]), sizeof(uint32_t));
  in.read(byte_ptr(&cp.max_readlen), sizeof(uint32_t));
  in.read(byte_ptr(&cp.paired_id_code), sizeof(uint8_t));
  cp.paired_id_match = read_bool(in);
  in.read(byte_ptr(&cp.num_reads_per_block), sizeof(int));
  in.read(byte_ptr(&cp.num_reads_per_block_long), sizeof(int));
  in.read(byte_ptr(&cp.num_thr), sizeof(int));
  in.read(byte_ptr(&cp.compression_level), sizeof(int));
  in.read(reinterpret_cast<char *>(cp.file_len_seq_thr),
          sizeof(uint64_t) * compression_params::kFileLenThrSize);
  in.read(reinterpret_cast<char *>(cp.file_len_id_thr),
          sizeof(uint64_t) * compression_params::kFileLenThrSize);
}

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
};

enum class input_record_format : uint8_t { fastq, fasta };

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

std::string shell_quote(const std::string &value) {
#ifdef _WIN32
  // On Windows, use generic_string() (forward slashes) for relative/absolute
  // paths passed to system(). cmd.exe and Windows APIs usually handle them.
  std::string quoted = "\"";
  for (const char character : value) {
    if (character == '"') {
      quoted += "\\\"";
    } else {
      quoted += character;
    }
  }
  quoted += '"';
  return quoted;
#else
  std::string quoted = "'";
  for (const char character : value) {
    if (character == '\'') {
      quoted += "'\"'\"'";
    } else {
      quoted += character;
    }
  }
  quoted += "'";
  return quoted;
#endif
}

std::string shell_path(const std::string &value) {
  return std::filesystem::path(value).generic_string();
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
         " --decompress --force --output " + shell_quote(shell_path(output_path)) +
         " --decoder-parallelism " + std::to_string(decoder_parallelism) + " " +
         shell_quote(shell_path(input_path));
}

void decompress_gzip_input_file_with_zlib(const std::string &input_path,
                                          const std::string &output_path) {
  gzip_istream gzip_input(input_path);
  if (!gzip_input.is_open()) {
    throw std::runtime_error("Failed opening gzipped compression input: " +
                             input_path);
  }

  std::ofstream output_stream(output_path, std::ios::binary);
  if (!output_stream.is_open()) {
    throw std::runtime_error("Failed opening staged compression input: " +
                             output_path);
  }

  std::vector<char> buffer(1 << 15);
  while (gzip_input.read(buffer.data(), buffer.size()) ||
         gzip_input.gcount() > 0) {
    output_stream.write(buffer.data(), gzip_input.gcount());
  }

  if (!output_stream) {
    throw std::runtime_error("Failed writing staged compression input: " +
                             output_path);
  }
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
      .input_2_was_gzipped = false};

  const bool input_1_is_gzipped = is_gzip_input_path(io_config.input_path_1);
  if (input_1_is_gzipped) {
    prepared_inputs.input_path_1 =
        decompressed_input_path(temp_dir, io_config.input_path_1, 1);
    decompress_gzip_input_file(io_config.input_path_1,
                               prepared_inputs.input_path_1, num_thr);
    prepared_inputs.input_1_was_gzipped = true;
  }

  if (!io_config.paired_end) {
    return prepared_inputs;
  }

  const bool input_2_is_gzipped = is_gzip_input_path(io_config.input_path_2);
  if (input_2_is_gzipped) {
    prepared_inputs.input_path_2 =
        decompressed_input_path(temp_dir, io_config.input_path_2, 2);
    decompress_gzip_input_file(io_config.input_path_2,
                               prepared_inputs.input_path_2, num_thr);
    prepared_inputs.input_2_was_gzipped = true;
  }

  return prepared_inputs;
}

void cleanup_prepared_compression_inputs(
    const prepared_compression_inputs &prepared_inputs) {
  if (prepared_inputs.input_1_was_gzipped) {
    std::filesystem::remove(prepared_inputs.input_path_1);
  }
  if (prepared_inputs.input_2_was_gzipped) {
    std::filesystem::remove(prepared_inputs.input_path_2);
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

  if (output_paths.size() != 1)
    throw std::runtime_error("Number of output files not equal to 1");
  io_config.archive_path = output_paths[0];
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
  std::cout << label << ": " << get_directory_size(temp_dir) << "\n";
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

  std::cout << "\n";
  std::cout << "Sizes of streams after compression: \n";
  std::cout << "Reads:      " << std::setw(12) << size_read << " bytes\n";
  std::cout << "Quality:    " << std::setw(12) << size_quality << " bytes\n";
  std::cout << "ID:         " << std::setw(12) << size_id << " bytes\n";
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
      std::cerr << "WARNING: Two output files provided for single end data. "
                   "Output will be written to the first file provided.";
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
              const int &compression_level) {
  omp_set_dynamic(0);

  std::cout << "Starting compression...\n";
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
      cleanup_prepared_compression_inputs(prepared_inputs);
      throw std::runtime_error(
          "Paired-end inputs must both be FASTQ or both be FASTA.");
    }
    input_format = input_format_2;
  }
  const bool fasta_input = input_format == input_record_format::fasta;
  const bool preserve_order = !pairing_only_flag;
  const bool preserve_id = !no_ids_flag;
  const bool preserve_quality = !no_quality_flag && !fasta_input;

  const uint32_t max_read_length =
      detect_max_read_length(prepared_inputs.input_path_1,
                             prepared_inputs.input_path_2, io_config.paired_end,
                             fasta_input);
  const bool long_flag = max_read_length > MAX_READ_LEN;

  if (long_flag) {
    std::cout << "Auto-detected long-read mode (max read length: "
              << max_read_length << ").\n";
  } else {
    std::cout << "Auto-detected short-read mode.\n";
  }

  compression_params cp{};
  cp.paired_end = io_config.paired_end;
  cp.preserve_order = preserve_order;
  cp.preserve_id = preserve_id;
  cp.preserve_quality = preserve_quality;
  cp.long_flag = long_flag;
  cp.num_reads_per_block = NUM_READS_PER_BLOCK;
  cp.num_reads_per_block_long = NUM_READS_PER_BLOCK_LONG;
  cp.num_thr = num_thr;
  cp.compression_level = compression_level;

  if (preserve_quality)
    configure_quality_options(cp, quality_options);

  std::cout << "Detected input format: " << input_format_name(input_format)
            << "\n";
  if (fasta_input) {
    std::cout << "FASTA input detected; quality values will not be stored.\n";
  }

  if (prepared_inputs.input_1_was_gzipped ||
      prepared_inputs.input_2_was_gzipped) {
    std::cout << "Detected gzipped input; decompressing to temporary input "
                 "files before compression.\n";
  }

  run_timed_step("Preprocessing ...", "Preprocessing", [&] {
    preprocess(prepared_inputs.input_path_1, prepared_inputs.input_path_2,
               temp_dir, cp, fasta_input);
  });
  cleanup_prepared_compression_inputs(prepared_inputs);
  print_temp_dir_size(temp_dir);

  if (!long_flag) {
    run_timed_step("Reordering ...", "Reordering",
                   [&] { call_reorder(temp_dir, cp); });

    print_temp_dir_size(temp_dir, "temp_dir size");

    run_timed_step("Encoding ...", "Encoding",
                   [&] { call_encoder(temp_dir, cp); });
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
                   "Reordering and compressing streams",
                   [&] { reorder_compress_streams(temp_dir, cp); });
    print_temp_dir_size(temp_dir);
  }

  std::string compression_params_path = temp_dir + "/cp.bin";
  std::ofstream compression_params_output(compression_params_path,
                                          std::ios::binary);
  write_compression_params(compression_params_output, cp);
  compression_params_output.close();

  print_compressed_stream_sizes(temp_dir);

  run_timed_step("Creating tar archive ...", "Tar archive", [&] {
    const std::string tar_command =
        "tar -cf " + shell_quote(shell_path(io_config.archive_path)) + " -C " +
        shell_quote(shell_path(temp_dir)) + " .";
    run_system_command_or_throw(
        tar_command, "Error occurred during tar archive generation.");
  });

  const auto compression_end = clock_type::now();
  std::cout << "Compression done!\n";
  std::cout << "Total time for compression: "
            << std::chrono::duration_cast<std::chrono::seconds>(
                   compression_end - compression_start)
                   .count()
            << " s\n";

  namespace fs = std::filesystem;
  fs::path archive_file_path{io_config.archive_path};
  std::cout << "\n";
  std::cout << "Total size: " << std::setw(12)
            << fs::file_size(archive_file_path) << " bytes\n";
  return;
}

void decompress(const std::string &temp_dir,
                const std::vector<std::string> &input_paths,
                const std::vector<std::string> &output_paths,
                const int &num_thr,
                const int &compression_level) {
  omp_set_dynamic(0);

  std::cout << "Starting decompression...\n";
  const auto decompression_start = clock_type::now();
  compression_params cp{};

  if (input_paths.size() != 1)
    throw std::runtime_error("Number of input files not equal to 1");

  run_timed_step("Untarring tar archive ...", "Untarring archive", [&] {
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

  bool paired_end = cp.paired_end;
  bool long_flag = cp.long_flag;
  const decompression_io_config io_config =
      resolve_decompression_io(input_paths, output_paths, paired_end);



  // Long-read and short-read archives diverge only at the reconstruction step.
  run_timed_step("Decompressing ...", "Decompressing", [&] {
    if (long_flag)
      decompress_long(temp_dir, io_config.output_path_1,
                      io_config.output_path_2, cp, num_thr,
                      compression_level);
    else
      decompress_short(temp_dir, io_config.output_path_1,
                       io_config.output_path_2, cp, num_thr,
                       compression_level);
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
    static thread_local std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<size_t> distribution(0, max_index - 1);
    return charset[distribution(generator)];
  };
  std::string random_value(length, 0);
  std::generate_n(random_value.begin(), length, random_char);
  return random_value;
}

} // namespace spring
