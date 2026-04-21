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
#include <unordered_map>
#include <omp.h>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "decompress.h"
#include "fs_utils.h"
#include "io_utils.h"
#include "paired_end_order.h"
#include "params.h"
#include "parse_utils.h"
#include "preprocess.h"
#include "progress.h"
#include "reordered_quality_id.h"
#include "reordered_streams.h"
#include "spring.h"
#include "template_dispatch.h"
#include "assay_detector.h"

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

constexpr const char *kBundleManifestName = "bundle.meta";
constexpr const char *kBundleVersion = "SPRING2_BUNDLE_V1";

struct bundle_manifest {
  std::string version;
  std::string read_archive_name;
  bool has_r3 = false;
  std::string read3_archive_name;
  std::string read3_alias_source;
  bool has_index = false;
  std::string index_archive_name;
  bool has_i2 = false;
  std::string r1_name;
  std::string r2_name;
  std::string r3_name;
  std::string i1_name;
  std::string i2_name;
};

std::string trim_ascii_whitespace(const std::string &input) {
  size_t begin = 0;
  while (begin < input.size() &&
         (input[begin] == ' ' || input[begin] == '\t' ||
          input[begin] == '\r' || input[begin] == '\n')) {
    begin++;
  }
  size_t end = input.size();
  while (end > begin &&
         (input[end - 1] == ' ' || input[end - 1] == '\t' ||
          input[end - 1] == '\r' || input[end - 1] == '\n')) {
    end--;
  }
  return input.substr(begin, end - begin);
}

std::string default_archive_name_from_input(const std::string &input_path) {
  std::filesystem::path p = std::filesystem::path(input_path).filename();
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
  return p.string();
}

bool paths_refer_to_same_file(const std::string &left,
                              const std::string &right) {
  std::error_code ec;
  const std::filesystem::path left_path(left);
  const std::filesystem::path right_path(right);
  if (!std::filesystem::exists(left_path, ec) || ec)
    return false;
  ec.clear();
  if (!std::filesystem::exists(right_path, ec) || ec)
    return false;
  ec.clear();
  return std::filesystem::equivalent(left_path, right_path, ec) && !ec;
}

void write_bundle_manifest(const std::string &manifest_path,
                           const bundle_manifest &manifest) {
  std::ofstream output(manifest_path, std::ios::binary);
  if (!output.is_open()) {
    throw std::runtime_error("Unable to write bundle manifest: " +
                             manifest_path);
  }
  output << "version=" << manifest.version << "\n";
  output << "read_archive=" << manifest.read_archive_name << "\n";
  output << "has_r3=" << (manifest.has_r3 ? "1" : "0") << "\n";
  output << "read3_archive=" << manifest.read3_archive_name << "\n";
  output << "read3_alias_source=" << manifest.read3_alias_source << "\n";
  output << "has_index=" << (manifest.has_index ? "1" : "0") << "\n";
  output << "index_archive=" << manifest.index_archive_name << "\n";
  output << "has_i2=" << (manifest.has_i2 ? "1" : "0") << "\n";
  output << "r1_name=" << manifest.r1_name << "\n";
  output << "r2_name=" << manifest.r2_name << "\n";
  output << "r3_name=" << manifest.r3_name << "\n";
  output << "i1_name=" << manifest.i1_name << "\n";
  output << "i2_name=" << manifest.i2_name << "\n";
}

bundle_manifest read_bundle_manifest(const std::string &manifest_path) {
  std::ifstream input(manifest_path, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("Unable to read bundle manifest: " +
                             manifest_path);
  }

  std::unordered_map<std::string, std::string> kv;
  std::string line;
  while (std::getline(input, line)) {
    const size_t delimiter = line.find('=');
    if (delimiter == std::string::npos)
      continue;
    const std::string key = trim_ascii_whitespace(line.substr(0, delimiter));
    const std::string value =
        trim_ascii_whitespace(line.substr(delimiter + 1));
    if (!key.empty())
      kv[key] = value;
  }

  bundle_manifest manifest;
  manifest.version = kv["version"];
  manifest.read_archive_name = kv["read_archive"];
  manifest.has_r3 = kv["has_r3"] == "1";
  manifest.read3_archive_name = kv["read3_archive"];
  manifest.read3_alias_source = kv["read3_alias_source"];
  manifest.has_index = kv["has_index"] == "1";
  manifest.index_archive_name = kv["index_archive"];
  manifest.has_i2 = kv["has_i2"] == "1";
  manifest.r1_name = kv["r1_name"];
  manifest.r2_name = kv["r2_name"];
  manifest.r3_name = kv["r3_name"];
  manifest.i1_name = kv["i1_name"];
  manifest.i2_name = kv["i2_name"];

  if (manifest.version != kBundleVersion || manifest.read_archive_name.empty() ||
      manifest.r1_name.empty()) {
    throw std::runtime_error("Invalid or unsupported bundle manifest format.");
  }
  if (manifest.has_r3 && manifest.r3_name.empty()) {
    throw std::runtime_error("Invalid bundle manifest: has_r3=1 but r3_name is missing.");
  }
  if (manifest.has_r3 && manifest.read3_archive_name.empty() &&
      manifest.read3_alias_source.empty()) {
    throw std::runtime_error("Invalid bundle manifest: has_r3=1 but no read3 archive or alias source.");
  }
  if (manifest.has_index && manifest.index_archive_name.empty()) {
    throw std::runtime_error("Invalid bundle manifest: has_index=1 but index archive is missing.");
  }
  if (manifest.has_index && manifest.i1_name.empty()) {
    throw std::runtime_error("Invalid bundle manifest: has_index=1 but i1_name is missing.");
  }
  if (manifest.has_i2 && manifest.i2_name.empty()) {
    throw std::runtime_error("Invalid bundle manifest: has_i2=1 but i2_name is missing.");
  }

  return manifest;
}

struct prepared_compression_inputs {
  std::string input_path_1;
  std::string input_path_2;
  bool input_1_was_gzipped;
  bool input_2_was_gzipped;
  bool input_1_actual_was_gzipped = false;
  bool input_2_actual_was_gzipped = false;
};

void decompress_gzip_input_file(const std::string &input_path,
                                const std::string &output_path, int num_thr);

enum class input_record_format : uint8_t { fastq, fasta };

void print_step_summary(const char *step_name,
                        const clock_type::time_point &step_start,
                        const clock_type::time_point &step_end) {
  SPRING_LOG_INFO(std::string(step_name) + " done!");
  SPRING_LOG_INFO(
      "Time for this step: " +
      std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                         step_end - step_start)
                         .count()) +
      " s");
}

template <typename Func>
void run_timed_step(const char *start_message, const char *step_name,
                    Func &&step) {
  SPRING_LOG_INFO(start_message);
  const auto step_start = clock_type::now();
  std::forward<Func>(step)();
  const auto step_end = clock_type::now();
  print_step_summary(step_name, step_start, step_end);
}

void run_system_command_or_throw(const std::string &command,
                                 const char *error_message) {
  SPRING_LOG_DEBUG("Executing system command: " + command);
#ifdef _WIN32
  std::string wrapped_command = "\"" + command + "\"";
  const int command_status = std::system(wrapped_command.c_str());
#else
  const int command_status = std::system(command.c_str());
#endif
  if (command_status != 0)
    throw std::runtime_error(error_message);
}

std::string to_ascii_lowercase(std::string value) {
  for (char &character : value) {
    character =
        static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  }
  return value;
}

[[nodiscard]] bool is_gzip_input_path(const std::string &input_path) {
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
  SPRING_LOG_DEBUG("Using zlib fallback to decompress staged input: " +
                    input_path + " -> " + output_path);
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
    SPRING_LOG_DEBUG("Attempting staged gzip decompression with rapidgzip.");
    const std::string rapidgzip_command =
        build_rapidgzip_command(input_path, output_path, num_thr);
#ifdef _WIN32
    std::string wrapped_command = "\"" + rapidgzip_command + "\"";
    if (std::system(wrapped_command.c_str()) == 0) {
      SPRING_LOG_DEBUG("Staged gzip decompression completed via rapidgzip.");
      return;
    }
#else
    if (std::system(rapidgzip_command.c_str()) == 0) {
      SPRING_LOG_DEBUG("Staged gzip decompression completed via rapidgzip.");
      return;
    }
#endif
    SPRING_LOG_DEBUG("rapidgzip invocation failed; falling back to zlib.");
  }

  decompress_gzip_input_file_with_zlib(input_path, output_path);
}

prepared_compression_inputs
prepare_compression_inputs(const compression_io_config &io_config,
                           const std::string &temp_dir, const int num_thr) {
  SPRING_LOG_DEBUG("Preparing compression inputs in temp directory: " +
                    temp_dir);
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
    SPRING_LOG_DEBUG("Input 1 is gzip-compressed; staging to: " +
                      prepared_inputs.input_path_1);
    decompress_gzip_input_file(io_config.input_path_1,
                               prepared_inputs.input_path_1, num_thr);
    prepared_inputs.input_1_actual_was_gzipped = true;
  }

  if (io_config.paired_end) {
    const bool input_2_is_gzipped = is_gzip_input_path(io_config.input_path_2);
    if (input_2_is_gzipped) {
      prepared_inputs.input_path_2 =
          decompressed_input_path(temp_dir, io_config.input_path_2, 2);
      SPRING_LOG_DEBUG("Input 2 is gzip-compressed; staging to: " +
                        prepared_inputs.input_path_2);
      decompress_gzip_input_file(io_config.input_path_2,
                                 prepared_inputs.input_path_2, num_thr);
      prepared_inputs.input_2_actual_was_gzipped = true;
    }
  }

  return prepared_inputs;
}

void cleanup_prepared_compression_inputs(
    const prepared_compression_inputs &prepared_inputs,
    bool pairing_only_flag) {
  if (!pairing_only_flag) {
    if (prepared_inputs.input_1_actual_was_gzipped) {
      SPRING_LOG_DEBUG("Removing staged input 1 file: " +
                        prepared_inputs.input_path_1);
      safe_remove_file(prepared_inputs.input_path_1);
    }
    if (prepared_inputs.input_2_actual_was_gzipped) {
      SPRING_LOG_DEBUG("Removing staged input 2 file: " +
                        prepared_inputs.input_path_2);
      safe_remove_file(prepared_inputs.input_path_2);
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

  SPRING_LOG_DEBUG("Resolved compression I/O: paired_end=" +
                    std::string(io_config.paired_end ? "true" : "false") +
                    ", input1=" + io_config.input_path_1 +
                    (io_config.paired_end
                         ? (", input2=" + io_config.input_path_2)
                         : std::string()) +
                    ", archive=" + io_config.archive_path);
  return io_config;
}

void configure_quality_options(compression_params &compression_params,
                               const string_list &quality_options) {
  if (quality_options.empty() || quality_options[0] == "lossless") {
    compression_params.quality.qvz_flag = false;
    compression_params.quality.ill_bin_flag = false;
    compression_params.quality.bin_thr_flag = false;
    SPRING_LOG_DEBUG("Quality mode set to lossless.");
    return;
  }

  if (quality_options[0] == "qvz") {
    if (quality_options.size() != 2)
      throw std::runtime_error("Invalid quality options.");

    compression_params.quality.qvz_ratio = parse_double_or_throw(
        quality_options[1], "Invalid qvz ratio provided.");
    if (compression_params.quality.qvz_ratio == 0.0)
      throw std::runtime_error("Invalid qvz ratio provided.");

    compression_params.quality.qvz_flag = true;
    compression_params.quality.ill_bin_flag = false;
    compression_params.quality.bin_thr_flag = false;
    SPRING_LOG_DEBUG("Quality mode set to qvz with ratio=" +
                      std::to_string(compression_params.quality.qvz_ratio));
    return;
  }

  if (quality_options[0] == "ill_bin") {
    compression_params.quality.ill_bin_flag = true;
    compression_params.quality.qvz_flag = false;
    compression_params.quality.bin_thr_flag = false;
    SPRING_LOG_DEBUG("Quality mode set to ill_bin.");
    return;
  }

  if (quality_options[0] == "binary") {
    if (quality_options.size() != 4)
      throw std::runtime_error("Invalid quality options.");

    compression_params.quality.bin_thr_thr = parse_int_or_throw(
        quality_options[1], "Invalid binary quality threshold.");
    compression_params.quality.bin_thr_high = parse_int_or_throw(
        quality_options[2], "Invalid binary high quality value.");
    compression_params.quality.bin_thr_low = parse_int_or_throw(
        quality_options[3], "Invalid binary low quality value.");
    if (compression_params.quality.bin_thr_thr > 94 ||
        compression_params.quality.bin_thr_high > 94 ||
        compression_params.quality.bin_thr_low > 94) {
      throw std::runtime_error(
          "Binary quality options must be in the range [0, 94].");
    }
    if (compression_params.quality.bin_thr_high <
            compression_params.quality.bin_thr_thr ||
        compression_params.quality.bin_thr_low >
            compression_params.quality.bin_thr_thr ||
        compression_params.quality.bin_thr_high <
            compression_params.quality.bin_thr_low) {
      throw std::runtime_error("Options do not satisfy low <= thr <= high.");
    }

    compression_params.quality.qvz_flag = false;
    compression_params.quality.ill_bin_flag = false;
    compression_params.quality.bin_thr_flag = true;
    SPRING_LOG_DEBUG(
      "Quality mode set to binary (thr=" +
      std::to_string(compression_params.quality.bin_thr_thr) +
      ", high=" + std::to_string(compression_params.quality.bin_thr_high) +
      ", low=" + std::to_string(compression_params.quality.bin_thr_low) +
      ").");
    return;
  }

  throw std::runtime_error("Invalid quality options.");
}

void print_temp_dir_size(const std::string &temp_dir,
                         const char *label = "Temporary directory size") {
  SPRING_LOG_INFO(std::string(label) + ": " +
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

  SPRING_LOG_INFO("");
  SPRING_LOG_INFO("Sizes of streams after compression: ");
  SPRING_LOG_INFO("Reads:      " + std::to_string(size_read) + " bytes");
  SPRING_LOG_INFO("Quality:    " + std::to_string(size_quality) + " bytes");
  SPRING_LOG_INFO("ID:         " + std::to_string(size_id) + " bytes");
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

  SPRING_LOG_DEBUG("Resolved decompression I/O: archive=" +
                    io_config.archive_path + ", output1=" +
                    io_config.output_path_1 +
                    (paired_end ? (", output2=" + io_config.output_path_2)
                                : std::string()));

  return io_config;
}

} // namespace

void perform_audit(const std::string &archive_path,
                   const std::string &temp_dir) {
  // Create a sub-directory for the audit untar to avoid clobbering existing
  // temp files
  std::string audit_dir = temp_dir + "/audit_extract";
  std::filesystem::create_directories(audit_dir);
  SPRING_LOG_DEBUG("Audit started for archive: " + archive_path +
                    " (extract dir: " + audit_dir + ")");

  try {
    extract_tar_archive(archive_path, audit_dir);

    const std::string manifest_path = audit_dir + "/" + kBundleManifestName;
    if (std::filesystem::exists(manifest_path)) {
      const bundle_manifest manifest = read_bundle_manifest(manifest_path);
      const std::string read_archive_path = audit_dir + "/" + manifest.read_archive_name;
      const std::string index_archive_path =
          manifest.has_index ? (audit_dir + "/" + manifest.index_archive_name)
                             : std::string();
      const std::string read3_archive_path =
          (manifest.has_r3 && manifest.read3_alias_source.empty())
              ? (audit_dir + "/" + manifest.read3_archive_name)
              : std::string();

      perform_audit(read_archive_path, temp_dir + "/audit_read_group");
      if (!read3_archive_path.empty()) {
        perform_audit(read3_archive_path, temp_dir + "/audit_read3_group");
      }
      if (!index_archive_path.empty()) {
        perform_audit(index_archive_path, temp_dir + "/audit_index_group");
      }

      std::cout << "Audit successful: grouped archive members are valid."
                << std::endl;
      std::filesystem::remove_all(audit_dir);
      return;
    }

    std::string compression_params_path = audit_dir + "/cp.bin";
    std::ifstream compression_params_input(compression_params_path,
                                           std::ios::binary);
    if (!compression_params_input.is_open())
      throw std::runtime_error("Can't open parameter file in audit.");

    compression_params cp{};
    read_compression_params(compression_params_input, cp);
    compression_params_input.close();

    NullDecompressionSink sink;
    if (cp.encoding.long_flag) {
      decompress_long(audit_dir, sink, cp);
    } else {
      decompress_short(audit_dir, sink, cp);
    }

    const bool is_lossless =
        cp.encoding.preserve_order && cp.encoding.preserve_quality &&
        cp.encoding.preserve_id && !cp.quality.qvz_flag &&
        !cp.quality.ill_bin_flag && !cp.quality.bin_thr_flag;

    if (is_lossless) {
      uint32_t seq_crc[2], qual_crc[2], id_crc[2];
      sink.get_digests(seq_crc, qual_crc, id_crc);
      bool mismatch = false;
      for (int i = 0; i < (cp.encoding.paired_end ? 2 : 1); ++i) {
        if (cp.read_info.sequence_crc[i] != 0 &&
            seq_crc[i] != cp.read_info.sequence_crc[i]) {
          Logger::log_error("Stream " + std::to_string(i + 1) +
                            " sequence digest mismatch: expected=" +
                            std::to_string(cp.read_info.sequence_crc[i]) +
                            " actual=" + std::to_string(seq_crc[i]));
          std::cerr << "Stream " << (i + 1) << " sequence digest mismatch!\n";
          mismatch = true;
        }
        if (cp.read_info.quality_crc[i] != 0 &&
            qual_crc[i] != cp.read_info.quality_crc[i]) {
          Logger::log_error("Stream " + std::to_string(i + 1) +
                            " quality digest mismatch: expected=" +
                            std::to_string(cp.read_info.quality_crc[i]) +
                            " actual=" + std::to_string(qual_crc[i]));
          std::cerr << "Stream " << (i + 1) << " quality digest mismatch!\n";
          mismatch = true;
        }
        if (cp.read_info.id_crc[i] != 0 &&
            id_crc[i] != cp.read_info.id_crc[i]) {
          Logger::log_error("Stream " + std::to_string(i + 1) +
                            " ID digest mismatch: expected=" +
                            std::to_string(cp.read_info.id_crc[i]) +
                            " actual=" + std::to_string(id_crc[i]));
          std::cerr << "Stream " << (i + 1) << " ID digest mismatch!\n";
          mismatch = true;
        }
      }

      if (mismatch)
        throw std::runtime_error("Archive integrity verification failed.");

      std::cout << "Audit successful: Data and Digests match perfectly."
                << std::endl;
    } else {
      std::cout << "Audit skipped: Record-level hashing is only supported for "
                   "lossless, ordered archives."
                << std::endl;
    }

    std::filesystem::remove_all(audit_dir);
  } catch (...) {
    std::error_code ec;
    std::filesystem::remove_all(audit_dir, ec);
    throw;
  }
}

void compress(const std::string &temp_dir,
              const std::vector<std::string> &input_paths,
              const std::vector<std::string> &output_paths, const int num_thr,
              const bool pairing_only_flag, const bool no_quality_flag,
              const bool no_ids_flag,
              const std::vector<std::string> &quality_options,
              const int compression_level, const std::string &note,
              const log_level verbosity_level, const bool audit_flag,
              const std::string &r3_path, const std::string &i1_path,
              const std::string &i2_path, const std::string &assay_type) {
  Logger::set_level(verbosity_level);
  ProgressBar progress(verbosity_level == log_level::quiet);
  ProgressBar::SetGlobalInstance(&progress);
  omp_set_dynamic(0);

  SPRING_LOG_INFO("Starting compression...");
  SPRING_LOG_DEBUG("Compression request: temp_dir=" + temp_dir +
                    ", num_threads=" + std::to_string(num_thr) +
                    ", level=" + std::to_string(compression_level) +
                    ", strip_order=" +
                    std::string(pairing_only_flag ? "true" : "false") +
                    ", strip_quality=" +
                    std::string(no_quality_flag ? "true" : "false") +
                    ", strip_ids=" +
                    std::string(no_ids_flag ? "true" : "false") +
                    ", audit=" + std::string(audit_flag ? "true" : "false"));

  const bool has_r3 = !r3_path.empty();
  const bool has_i1 = !i1_path.empty();
  const bool has_i2 = !i2_path.empty();
  const bool grouped_bundle = has_r3 || has_i1 || has_i2;

  if (grouped_bundle) {
    if (input_paths.size() < 2) {
      throw std::runtime_error(
          "Grouped compression requires at least R1 and R2.");
    }
    if (output_paths.size() > 1) {
      throw std::runtime_error(
          "Number of output files not equal to 1 for grouped compression.");
    }

    const std::string output_archive_path =
        output_paths.empty() ? default_archive_name_from_input(input_paths[0])
                             : output_paths[0];
    const std::string read_archive_name = "reads_group.sp";
    const std::string read3_archive_name = "read3_group.sp";
    const std::string index_archive_name = "index_group.sp";
    const std::string bundle_dir = temp_dir + "/bundle";
    const std::string read_work_dir = temp_dir + "/bundle_read_work";
    const std::string read3_work_dir = temp_dir + "/bundle_read3_work";
    const std::string index_work_dir = temp_dir + "/bundle_index_work";

    std::filesystem::create_directories(bundle_dir);
    std::filesystem::create_directories(read_work_dir);
    if (has_r3) {
      std::filesystem::create_directories(read3_work_dir);
    }
    if (has_i1) {
      std::filesystem::create_directories(index_work_dir);
    }

    const string_list read_inputs = {input_paths[0], input_paths[1]};
    string_list read3_inputs;
    string_list index_inputs;
    if (has_r3) {
      read3_inputs.push_back(r3_path);
    }
    if (has_i1) {
      index_inputs.push_back(i1_path);
      if (has_i2) {
        index_inputs.push_back(i2_path);
      }
    }

    SPRING_LOG_INFO("Detected grouped lanes; compressing as grouped bundle "
                    "(read pair + optional read3 + optional index pair).");

    // Compress R1/R2 as a regular SPRING archive.
    compress(read_work_dir, read_inputs,
             {bundle_dir + "/" + read_archive_name}, num_thr,
             pairing_only_flag, no_quality_flag, no_ids_flag, quality_options,
             compression_level, note, verbosity_level, audit_flag, "", "", "",
             assay_type);

    std::string read3_alias_source;
    if (has_r3) {
      if (paths_refer_to_same_file(r3_path, input_paths[0])) {
        read3_alias_source = "R1";
      } else if (paths_refer_to_same_file(r3_path, input_paths[1])) {
        read3_alias_source = "R2";
      } else {
        compress(read3_work_dir, read3_inputs,
                 {bundle_dir + "/" + read3_archive_name}, num_thr,
                 pairing_only_flag, no_quality_flag, no_ids_flag,
                 quality_options, compression_level,
                 note.empty() ? std::string("read3-group")
                              : (note + " | read3-group"),
                 verbosity_level, audit_flag, "", "", "", assay_type);
      }
    }

    if (has_i1) {
      compress(index_work_dir, index_inputs,
               {bundle_dir + "/" + index_archive_name}, num_thr,
               pairing_only_flag, no_quality_flag, no_ids_flag,
               quality_options, compression_level,
               note.empty() ? std::string("index-group")
                            : (note + " | index-group"),
               verbosity_level, audit_flag, "", "", "", assay_type);
    }

    std::error_code ec;
    std::filesystem::remove_all(read_work_dir, ec);
    ec.clear();
    if (has_r3) {
      std::filesystem::remove_all(read3_work_dir, ec);
      ec.clear();
    }
    if (has_i1) {
      std::filesystem::remove_all(index_work_dir, ec);
    }

    const bundle_manifest manifest{
        .version = kBundleVersion,
        .read_archive_name = read_archive_name,
        .has_r3 = has_r3,
        .read3_archive_name = has_r3 && read3_alias_source.empty()
                                 ? read3_archive_name
                                 : std::string(),
        .read3_alias_source = read3_alias_source,
        .has_index = has_i1,
        .index_archive_name = has_i1 ? index_archive_name : std::string(),
        .has_i2 = has_i2,
        .r1_name = std::filesystem::path(input_paths[0]).filename().string(),
        .r2_name = std::filesystem::path(input_paths[1]).filename().string(),
        .r3_name = has_r3 ? std::filesystem::path(r3_path).filename().string()
                          : std::string(),
        .i1_name = has_i1 ? std::filesystem::path(i1_path).filename().string()
                          : std::string(),
        .i2_name = has_i2 ? std::filesystem::path(i2_path).filename().string()
                          : std::string()};
    write_bundle_manifest(bundle_dir + "/" + kBundleManifestName, manifest);

    run_timed_step("Creating grouped bundle archive ...", "Tar archive", [&] {
      progress.set_stage("Creating archive", 0.95F, 1.0F);
      create_tar_archive(output_archive_path, bundle_dir);
    });
    SPRING_LOG_DEBUG("Grouped archive created at: " + output_archive_path);

    if (audit_flag) {
      SPRING_LOG_DEBUG("Running post-compression audit for grouped archive.");
      perform_audit(output_archive_path, temp_dir);
    }

    ProgressBar::SetGlobalInstance(nullptr);
    return;
  }

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
  bool contains_non_acgtn_symbols = false;
  const uint32_t max_read_length = detect_max_read_length(
      prepared_inputs.input_path_1, prepared_inputs.input_path_2,
      io_config.paired_end, fasta_input, use_crlf,
      contains_non_acgtn_symbols);
  const bool long_flag =
      (max_read_length > MAX_READ_LEN) || contains_non_acgtn_symbols;
  SPRING_LOG_DEBUG("Detected maximum read length=" +
                    std::to_string(max_read_length) +
                    ", use_crlf=" + std::string(use_crlf ? "true" : "false") +
                    ", non_acgtn_symbols=" +
                    std::string(contains_non_acgtn_symbols ? "true" : "false") +
                    ", long_mode=" + std::string(long_flag ? "true" : "false"));

  if (contains_non_acgtn_symbols) {
    SPRING_LOG_INFO("Detected non-ACGTN symbols in read sequences; "
                    "switching to long-read mode to preserve sequence "
                    "alphabet losslessly.");
  }

  if (long_flag) {
    SPRING_LOG_INFO("Auto-detected long-read mode.");
  } else {
    SPRING_LOG_INFO("Auto-detected short-read mode.");
  }

  compression_params cp{};
  cp.encoding.paired_end = io_config.paired_end;
  cp.encoding.preserve_order = preserve_order;
  cp.encoding.preserve_quality = preserve_quality;
  cp.encoding.preserve_id = preserve_id;
  cp.encoding.long_flag = long_flag;
  cp.encoding.use_crlf = use_crlf;
  cp.encoding.num_reads_per_block = NUM_READS_PER_BLOCK;
  cp.encoding.num_reads_per_block_long = NUM_READS_PER_BLOCK_LONG;
  cp.encoding.num_thr = num_thr;
  cp.encoding.compression_level = compression_level;
  cp.read_info.note = note;
  
  std::string final_assay = assay_type;
  std::string final_confidence = "N/A";
  if (assay_type == "auto") {
      SPRING_LOG_INFO("Running auto-detection for assay type on first 10,000 reads...");
      AssayDetector detector;
      final_assay = detector.detect(prepared_inputs.input_path_1, 
                                    io_config.paired_end ? prepared_inputs.input_path_2 : "", 
                                    r3_path, i1_path, i2_path, final_confidence);
      SPRING_LOG_INFO("Auto-detected assay: " + final_assay + " (confidence: " + final_confidence + ")");
  }

  cp.read_info.assay = final_assay;
  cp.read_info.assay_confidence = final_confidence;
  
  cp.encoding.fasta_mode = fasta_input;
  cp.read_info.input_filename_1 =
      std::filesystem::path(io_config.input_path_1).filename().string();
  if (io_config.paired_end) {
    cp.read_info.input_filename_2 =
        std::filesystem::path(io_config.input_path_2).filename().string();
  }

  SPRING_LOG_DEBUG(
      "Archive metadata inputs: name1='" + cp.read_info.input_filename_1 +
      "'" +
      (cp.encoding.paired_end
           ? (", name2='" + cp.read_info.input_filename_2 + "'")
           : std::string()));

  // Extract detailed gzip metadata for input 1
  extract_gzip_detailed_info(
      io_config.input_path_1, cp.gzip.streams[0].was_gzipped,
      cp.gzip.streams[0].flg, cp.gzip.streams[0].mtime, cp.gzip.streams[0].xfl,
      cp.gzip.streams[0].os, cp.gzip.streams[0].name,
      cp.gzip.streams[0].is_bgzf, cp.gzip.streams[0].bgzf_block_size,
      cp.gzip.streams[0].uncompressed_size, cp.gzip.streams[0].compressed_size,
      cp.gzip.streams[0].member_count);

  // Extract detailed gzip metadata for input 2 (if paired-end)
  if (io_config.paired_end) {
    extract_gzip_detailed_info(
        io_config.input_path_2, cp.gzip.streams[1].was_gzipped,
        cp.gzip.streams[1].flg, cp.gzip.streams[1].mtime,
        cp.gzip.streams[1].xfl, cp.gzip.streams[1].os, cp.gzip.streams[1].name,
        cp.gzip.streams[1].is_bgzf, cp.gzip.streams[1].bgzf_block_size,
        cp.gzip.streams[1].uncompressed_size,
        cp.gzip.streams[1].compressed_size, cp.gzip.streams[1].member_count);
  }

  if (preserve_quality)
    configure_quality_options(cp, quality_options);

  SPRING_LOG_INFO(std::string("Detected input format: ") +
                   input_format_name(input_format));
  if (fasta_input) {
    SPRING_LOG_INFO(
        "FASTA input detected; quality values will not be stored.");
  }

  if (prepared_inputs.input_1_actual_was_gzipped ||
      prepared_inputs.input_2_actual_was_gzipped) {
    SPRING_LOG_INFO("Detected gzipped input; decompressing to temporary input "
                     "files before compression.");
  }

  SPRING_LOG_DEBUG("Effective encoding options: paired_end=" +
                    std::string(cp.encoding.paired_end ? "true" : "false") +
                    ", preserve_order=" +
                    std::string(cp.encoding.preserve_order ? "true" : "false") +
                    ", preserve_id=" +
                    std::string(cp.encoding.preserve_id ? "true" : "false") +
                    ", preserve_quality=" +
                    std::string(cp.encoding.preserve_quality ? "true" : "false") +
                    ", fasta_mode=" +
                    std::string(cp.encoding.fasta_mode ? "true" : "false"));

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
    create_tar_archive(io_config.archive_path, temp_dir);
  });
  SPRING_LOG_DEBUG("Archive created at: " + io_config.archive_path);

  const auto compression_end = clock_type::now();
  if (Logger::is_info_enabled()) {
    std::cout << "Compression done!\n";
    std::cout << "Total time for compression: "
              << std::chrono::duration_cast<std::chrono::seconds>(
                     compression_end - compression_start)
                     .count()
              << " s\n";
  } else {
    progress.finalize();
  }

  if (Logger::is_info_enabled()) {
    namespace fs = std::filesystem;
    fs::path archive_file_path{io_config.archive_path};
    std::cout << "\n";
    std::cout << "Total size: " << std::setw(12)
              << fs::file_size(archive_file_path) << " bytes\n";
  }
  ProgressBar::SetGlobalInstance(nullptr);

  if (audit_flag) {
    SPRING_LOG_DEBUG("Running post-compression audit.");
    perform_audit(io_config.archive_path, temp_dir);
  }
}

void decompress(const std::string &temp_dir,
                const std::vector<std::string> &input_paths,
                const std::vector<std::string> &output_paths,
                const int num_thr, const int compression_level,
                const log_level verbosity_level, const bool unzip_flag) {
  Logger::set_level(verbosity_level);
  ProgressBar progress(verbosity_level == log_level::quiet);
  ProgressBar::SetGlobalInstance(&progress);
  omp_set_dynamic(0);

  SPRING_LOG_INFO("Starting decompression...");
  SPRING_LOG_DEBUG("Decompression request: temp_dir=" + temp_dir +
                    ", unzip=" + std::string(unzip_flag ? "true" : "false"));
  const auto decompression_start = clock_type::now();
  compression_params cp{};

  if (input_paths.size() != 1)
    throw std::runtime_error("Number of input files not equal to 1");

  run_timed_step("Untarring tar archive ...", "Untarring archive", [&] {
    progress.set_stage("Untarring", 0.0F, 0.10F);
    extract_tar_archive(input_paths[0], temp_dir);
  });

  const std::string manifest_path = temp_dir + "/" + kBundleManifestName;
  if (std::filesystem::exists(manifest_path)) {
    const bundle_manifest manifest = read_bundle_manifest(manifest_path);
    SPRING_LOG_INFO("Detected grouped bundle archive (reads + optional read3 + optional index reads).");

    std::vector<std::string> resolved_outputs;
    if (output_paths.empty()) {
      resolved_outputs.push_back(manifest.r1_name);
      resolved_outputs.push_back(manifest.r2_name);
      if (manifest.has_r3) {
        resolved_outputs.push_back(manifest.r3_name);
      }
      if (manifest.has_index) {
        resolved_outputs.push_back(manifest.i1_name);
        if (manifest.has_i2)
          resolved_outputs.push_back(manifest.i2_name);
      }

      for (size_t i = 0; i < resolved_outputs.size(); ++i) {
        for (size_t j = 0; j < i; ++j) {
          if (resolved_outputs[i] == resolved_outputs[j]) {
            resolved_outputs[i] += ".index";
            break;
          }
        }
      }
    } else if (output_paths.size() == 1) {
      resolved_outputs.push_back(output_paths[0] + ".R1");
      resolved_outputs.push_back(output_paths[0] + ".R2");
      if (manifest.has_r3) {
        resolved_outputs.push_back(output_paths[0] + ".R3");
      }
      if (manifest.has_index) {
        resolved_outputs.push_back(output_paths[0] + ".I1");
        if (manifest.has_i2)
          resolved_outputs.push_back(output_paths[0] + ".I2");
      }
    } else {
      size_t expected = 2;
      if (manifest.has_r3)
        expected += 1;
      if (manifest.has_index)
        expected += manifest.has_i2 ? 2 : 1;
      if (output_paths.size() != expected) {
        throw std::runtime_error(
            "Grouped archive decompression expects either 0 outputs, 1 output "
            "prefix, or exactly " +
            std::to_string(expected) + " output files.");
      }
      resolved_outputs = output_paths;
    }

    const std::string read_archive_path = temp_dir + "/" + manifest.read_archive_name;
    const std::string read3_archive_path =
      (manifest.has_r3 && manifest.read3_alias_source.empty())
        ? (temp_dir + "/" + manifest.read3_archive_name)
        : std::string();
    const std::string index_archive_path =
      manifest.has_index ? (temp_dir + "/" + manifest.index_archive_name)
                 : std::string();

    decompress(temp_dir + "/decompress_reads", {read_archive_path},
               {resolved_outputs[0], resolved_outputs[1]},
               num_thr, compression_level,
               verbosity_level, unzip_flag);

    size_t next_output_index = 2;
    if (manifest.has_r3) {
      if (!manifest.read3_alias_source.empty()) {
        const std::string &alias_source =
            (manifest.read3_alias_source == "R2") ? resolved_outputs[1]
                                                   : resolved_outputs[0];
        std::error_code copy_ec;
        std::filesystem::copy_file(
            alias_source, resolved_outputs[next_output_index],
            std::filesystem::copy_options::overwrite_existing, copy_ec);
        if (copy_ec) {
          throw std::runtime_error("Failed to materialize aliased R3 output: " +
                                   copy_ec.message());
        }
      } else {
        decompress(temp_dir + "/decompress_read3", {read3_archive_path},
                   {resolved_outputs[next_output_index]}, num_thr,
                   compression_level, verbosity_level, unzip_flag);
      }
      next_output_index++;
    }

    if (manifest.has_index) {
      if (manifest.has_i2) {
        decompress(temp_dir + "/decompress_index", {index_archive_path},
                   {resolved_outputs[next_output_index],
                    resolved_outputs[next_output_index + 1]},
                   num_thr, compression_level, verbosity_level, unzip_flag);
      } else {
        decompress(temp_dir + "/decompress_index", {index_archive_path},
                   {resolved_outputs[next_output_index]}, num_thr,
                   compression_level, verbosity_level, unzip_flag);
      }
    }

    ProgressBar::SetGlobalInstance(nullptr);
    return;
  }

  std::string compression_params_path = temp_dir + "/cp.bin";
  std::ifstream compression_params_input(compression_params_path,
                                         std::ios::binary);
  if (!compression_params_input.is_open())
    throw std::runtime_error("Can't open parameter file.");
  read_compression_params(compression_params_input, cp);
  if (!compression_params_input.good())
    throw std::runtime_error("Can't read compression parameters.");
  compression_params_input.close();
  if (num_thr > 0) {
    cp.encoding.num_thr = num_thr;
  }

  bool paired_end = cp.encoding.paired_end;
  SPRING_LOG_DEBUG("Archive metadata: paired_end=" +
                    std::string(cp.encoding.paired_end ? "true" : "false") +
                    ", long_mode=" +
                    std::string(cp.encoding.long_flag ? "true" : "false") +
                    ", preserve_order=" +
                    std::string(cp.encoding.preserve_order ? "true" : "false") +
                    ", preserve_quality=" +
                    std::string(cp.encoding.preserve_quality ? "true" : "false") +
                    ", preserve_id=" +
                    std::string(cp.encoding.preserve_id ? "true" : "false") +
                    ", fasta_mode=" +
                    std::string(cp.encoding.fasta_mode ? "true" : "false"));
  if (Logger::is_info_enabled()) {
    std::cout << "Original filenames detected in archive:\n";
    std::cout << "  Input 1: " << cp.read_info.input_filename_1 << "\n";
    if (paired_end) {
      std::cout << "  Input 2: " << cp.read_info.input_filename_2 << "\n";
    }

    if (!cp.read_info.note.empty()) {
      std::cout << "Note: " << cp.read_info.note << "\n";
    }
  }

  auto has_compressed_suffix = [](const std::string &path) {
    return path.ends_with(".gz");
  };

  auto strip_compressed_suffix = [](const std::string &path) {
    if (path.ends_with(".gz"))
      return path.substr(0, path.size() - 3);
    return path;
  };

  std::vector<std::string> resolved_output_paths = output_paths;
  if (resolved_output_paths.empty()) {
    if (cp.read_info.input_filename_1.empty()) {
      throw std::runtime_error("Input file 1 is required");
    }
    resolved_output_paths.push_back(cp.read_info.input_filename_1);
    if (cp.encoding.paired_end) {
      resolved_output_paths.push_back(cp.read_info.input_filename_2);
    }
  }

  bool should_gzip[2] = {false, false};
  bool should_bgzf[2] = {false, false};
  for (int i = 0; i < (cp.encoding.paired_end ? 2 : 1); i++) {
    const std::string &path = (i < resolved_output_paths.size())
                                  ? resolved_output_paths[i]
                                  : resolved_output_paths[0];
    bool is_bgzf =
        (i == 0) ? cp.gzip.streams[0].is_bgzf : cp.gzip.streams[1].is_bgzf;
    uint8_t xfl = (i == 0) ? cp.gzip.streams[0].xfl : cp.gzip.streams[1].xfl;

    if (unzip_flag)
      continue;

    if (has_compressed_suffix(path)) {
      should_gzip[i] = true;
      should_bgzf[i] = is_bgzf;
      if (xfl == 2)
        cp.encoding.compression_level = 9;
      else if (xfl == 4)
        cp.encoding.compression_level = 1;
    }
  }

  for (std::string &path : resolved_output_paths) {
    if (unzip_flag && has_compressed_suffix(path)) {
      path = strip_compressed_suffix(path);
    }
  }

  SPRING_LOG_DEBUG("Resolved " +
                    std::to_string(resolved_output_paths.size()) +
                    " output path(s) after unzip handling.");

  const decompression_io_config io_config =
      resolve_decompression_io(input_paths, resolved_output_paths, paired_end);

  run_timed_step("Decompressing ...", "Decompressing", [&] {
    progress.set_stage("Decompressing", 0.10F, 1.0F);
    FileDecompressionSink sink(io_config.output_path_1, io_config.output_path_2,
                               cp, cp.encoding.use_crlf, should_gzip,
                               should_bgzf);
    if (cp.encoding.long_flag) {
      decompress_long(temp_dir, sink, cp);
    } else {
      decompress_short(temp_dir, sink, cp);
    }

    // Integrity verification
    const bool is_lossless =
        cp.encoding.preserve_order && cp.encoding.preserve_quality &&
        cp.encoding.preserve_id && !cp.quality.qvz_flag &&
        !cp.quality.ill_bin_flag && !cp.quality.bin_thr_flag;

    if (is_lossless) {
      uint32_t seq_crc[2], qual_crc[2], id_crc[2];
      sink.get_digests(seq_crc, qual_crc, id_crc);
      bool mismatch = false;
      for (int i = 0; i < (cp.encoding.paired_end ? 2 : 1); ++i) {
        if (cp.read_info.sequence_crc[i] != 0 &&
            seq_crc[i] != cp.read_info.sequence_crc[i])
          mismatch = true;
        if (cp.read_info.quality_crc[i] != 0 &&
            qual_crc[i] != cp.read_info.quality_crc[i])
          mismatch = true;
        if (cp.read_info.id_crc[i] != 0 &&
            id_crc[i] != cp.read_info.id_crc[i])
          mismatch = true;
      }

      if (mismatch) {
        Logger::log_error("Integrity check failed during decompression.");
        throw std::runtime_error(
            "ARCHIVE INTEGRITY CHECK FAILED: Reconstructed data does not match "
            "original digests. The archive may be corrupted.");
      }
      SPRING_LOG_DEBUG("Integrity check passed for lossless archive.");
    }
  });

  const auto decompression_end = clock_type::now();
  if (Logger::is_info_enabled()) {
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

