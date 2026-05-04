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
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "assay_detector.h"
#include "bundle_manifest.h"
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
#include "version.h"

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

std::string normalized_path_key(const std::string &path) {
  std::error_code ec;
  std::filesystem::path normalized =
      std::filesystem::absolute(std::filesystem::path(path), ec);
  if (ec) {
    normalized = std::filesystem::path(path);
  }
  std::string key = normalized.lexically_normal().generic_string();
#ifdef _WIN32
  for (char &character : key) {
    character =
        static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
  }
#endif
  return key;
}

void validate_output_targets(const std::string &archive_path,
                             const std::vector<std::string> &output_paths) {
  std::unordered_set<std::string> seen_outputs;
  const std::string archive_key = normalized_path_key(archive_path);
  for (const std::string &output_path : output_paths) {
    const std::string output_key = normalized_path_key(output_path);
    if (!seen_outputs.insert(output_key).second) {
      throw std::runtime_error("Output paths must be distinct.");
    }
    if (output_key == archive_key) {
      throw std::runtime_error(
          "Output path must not overwrite the input archive.");
    }
  }
}

void validate_compression_target(const std::vector<std::string> &input_paths,
                                 const std::string &archive_path) {
  const std::string archive_key = normalized_path_key(archive_path);
  for (const std::string &input_path : input_paths) {
    if (normalized_path_key(input_path) == archive_key) {
      throw std::runtime_error(
          "Output archive path must not overwrite an input file.");
    }
  }
}

std::string assay_from_archive_metadata(const std::string &archive_path) {
  auto contents = read_files_from_tar_memory(archive_path, {"cp.bin"});
  if (!contents.contains("cp.bin")) {
    throw std::runtime_error("Could not find cp.bin in archive: " +
                             archive_path);
  }

  compression_params cp{};
  std::istringstream input(contents["cp.bin"], std::ios::binary);
  read_compression_params(input, cp);
  if (!input.good()) {
    throw std::runtime_error("Could not parse cp.bin in archive: " +
                             archive_path);
  }
  return cp.read_info.assay.empty() ? std::string("auto") : cp.read_info.assay;
}

std::string
assay_from_archive_metadata_bytes(const std::string &archive_bytes,
                                  const std::string &archive_label) {
  auto contents = read_files_from_tar_bytes(archive_bytes, {"cp.bin"});
  if (!contents.contains("cp.bin")) {
    throw std::runtime_error("Could not find cp.bin in archive: " +
                             archive_label);
  }

  compression_params cp{};
  std::istringstream input(contents["cp.bin"], std::ios::binary);
  read_compression_params(input, cp);
  if (!input.good()) {
    throw std::runtime_error("Could not parse cp.bin in archive: " +
                             archive_label);
  }
  return cp.read_info.assay.empty() ? std::string("auto") : cp.read_info.assay;
}

int gzip_output_compression_level(
    const compression_params::GzipMetadata::Stream &stream,
    const int default_level) {
  if (stream.xfl == 2)
    return 8;
  if (stream.xfl == 4)
    return 1;
  return default_level;
}

std::string append_group_role_suffix(const std::string &path,
                                     const std::string &suffix) {
  std::filesystem::path file_path(path);
  const std::filesystem::path parent = file_path.parent_path();
  std::string filename = file_path.filename().string();
  std::string trailing_gzip_suffix;
  if (filename.size() > 3 && filename.substr(filename.size() - 3) == ".gz") {
    trailing_gzip_suffix = ".gz";
    filename.erase(filename.size() - 3);
  }

  const std::string::size_type extension_pos = filename.find('.');
  std::string suffixed_name;
  if (extension_pos == std::string::npos) {
    suffixed_name = filename + "." + suffix + trailing_gzip_suffix;
  } else {
    suffixed_name = filename.substr(0, extension_pos) + "." + suffix +
                    filename.substr(extension_pos) + trailing_gzip_suffix;
  }
  return parent.empty() ? suffixed_name : (parent / suffixed_name).string();
}

std::vector<std::string>
build_default_grouped_output_paths(const bundle_manifest &manifest) {
  struct output_role {
    std::string name;
    std::string role;
  };

  std::vector<output_role> requested_outputs = {
      {manifest.r1_name, "R1"},
      {manifest.r2_name, "R2"},
  };
  if (manifest.has_r3) {
    requested_outputs.push_back({manifest.r3_name, "R3"});
  }
  if (manifest.has_index) {
    requested_outputs.push_back({manifest.i1_name, "I1"});
    if (manifest.has_i2) {
      requested_outputs.push_back({manifest.i2_name, "I2"});
    }
  }

  std::unordered_map<std::string, size_t> name_counts;
  for (const output_role &output : requested_outputs) {
    name_counts[normalized_path_key(output.name)]++;
  }

  std::unordered_set<std::string> used_paths;
  std::vector<std::string> resolved_outputs;
  resolved_outputs.reserve(requested_outputs.size());
  for (const output_role &output : requested_outputs) {
    std::string candidate = output.name;
    if (name_counts[normalized_path_key(output.name)] > 1) {
      candidate = append_group_role_suffix(output.name, output.role);
    }

    std::string candidate_key = normalized_path_key(candidate);
    size_t disambiguator = 2;
    while (!used_paths.insert(candidate_key).second) {
      candidate = append_group_role_suffix(
          output.name, output.role + std::to_string(disambiguator));
      candidate_key = normalized_path_key(candidate);
      disambiguator++;
    }
    resolved_outputs.push_back(candidate);
  }

  return resolved_outputs;
}

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

prepared_compression_inputs
prepare_compression_inputs(const compression_io_config &io_config,
                           const std::string &temp_dir, const int /*num_thr*/) {
  SPRING_LOG_DEBUG("Preparing compression inputs for direct streaming in: " +
                   temp_dir);
  prepared_compression_inputs prepared_inputs{
      .input_path_1 = io_config.input_path_1,
      .input_path_2 = io_config.input_path_2,
      .input_1_was_gzipped = is_gzip_input_path(io_config.input_path_1),
      .input_2_was_gzipped =
          io_config.paired_end && is_gzip_input_path(io_config.input_path_2)};

  return prepared_inputs;
}

void cleanup_prepared_compression_inputs(
    const prepared_compression_inputs & /*prepared_inputs*/,
    bool /*pairing_only_flag*/) {}

void recreate_compression_workdir(const std::string &temp_dir) {
  (void)temp_dir;
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
  const std::string normalized_path = strip_gzip_suffix(input_path);
  if (is_fastq_extension(normalized_path)) {
    detected = true;
    return input_record_format::fastq;
  }
  if (is_fasta_extension(normalized_path)) {
    detected = true;
    return input_record_format::fasta;
  }

  detected = false;
  return input_record_format::fastq;
}

input_record_format
detect_input_format_from_content(const std::string &input_path,
                                 bool &detected) {
  std::ifstream plain_stream;
  std::istream *input_stream = nullptr;
  std::unique_ptr<gzip_istream> gzip_stream;
  const bool gzip_enabled = is_gzip_input_path(input_path);
  if (gzip_enabled) {
    gzip_stream = std::make_unique<gzip_istream>(input_path);
    input_stream = gzip_stream.get();
  } else {
    plain_stream.open(input_path);
    input_stream = &plain_stream;
  }
  if (input_stream == nullptr || !(*input_stream)) {
    throw std::runtime_error("Error opening input file: " + input_path);
  }

  std::vector<std::string> non_empty_lines;
  std::string line;
  while (non_empty_lines.size() < 3 && std::getline(*input_stream, line)) {
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
  if (temp_dir.empty()) {
    return;
  }
  SPRING_LOG_INFO(std::string(label) + ": " +
                  std::to_string(get_directory_size(temp_dir)));
}

void print_compressed_stream_sizes(const std::string &temp_dir) {
  if (temp_dir.empty()) {
    return;
  }
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

void print_compressed_stream_sizes(
    const std::unordered_map<std::string, std::string> &archive_members) {
  uint64_t size_read = 0;
  uint64_t size_quality = 0;
  uint64_t size_id = 0;
  for (const auto &[name, contents] : archive_members) {
    if (name.empty()) {
      continue;
    }
    switch (name[0]) {
    case 'r':
      size_read += contents.size();
      break;
    case 'q':
      size_quality += contents.size();
      break;
    case 'i':
      size_id += contents.size();
      break;
    }
  }

  SPRING_LOG_INFO("");
  SPRING_LOG_INFO("Sizes of streams after compression: ");
  SPRING_LOG_INFO("Reads:      " + std::to_string(size_read) + " bytes");
  SPRING_LOG_INFO("Quality:    " + std::to_string(size_quality) + " bytes");
  SPRING_LOG_INFO("ID:         " + std::to_string(size_id) + " bytes");
}

void merge_archive_members(
    std::unordered_map<std::string, std::string> &archive_members,
    std::unordered_map<std::string, std::string> new_members) {
  archive_members.insert(std::make_move_iterator(new_members.begin()),
                         std::make_move_iterator(new_members.end()));
}

std::string serialize_compression_params(const compression_params &cp) {
  std::ostringstream output(std::ios::binary);
  write_compression_params(output, cp);
  return output.str();
}

std::vector<tar_archive_source> build_archive_sources(
    const std::unordered_map<std::string, std::string> &archive_members) {
  std::vector<tar_archive_source> sources;
  sources.reserve(archive_members.size());
  for (const auto &[name, contents] : archive_members) {
    tar_archive_source source;
    source.archive_path = name;
    source.contents = contents;
    source.from_memory = true;
    sources.push_back(std::move(source));
  }
  return sources;
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

  SPRING_LOG_DEBUG(
      "Resolved decompression I/O: archive=" + io_config.archive_path +
      ", output1=" + io_config.output_path_1 +
      (paired_end ? (", output2=" + io_config.output_path_2) : std::string()));

  return io_config;
}

} // namespace

namespace {

void perform_audit_standard_artifact(
    const decompression_archive_artifact &artifact,
    const std::string &archive_label) {
  SPRING_LOG_DEBUG("Audit (standard) started for archive: " + archive_label +
                   " (in-memory)");

  std::istringstream compression_params_input(artifact.require("cp.bin"),
                                              std::ios::binary);

  compression_params cp{};
  read_compression_params(compression_params_input, cp);
  if (!compression_params_input.good()) {
    throw std::runtime_error("Can't read parameter file in audit.");
  }

  NullDecompressionSink sink;
  if (cp.encoding.long_flag) {
    decompress_long(artifact, sink, cp, cp.encoding.num_thr);
  } else {
    decompress_short(artifact, sink, cp, cp.encoding.num_thr);
  }

  const bool is_lossless = cp.encoding.preserve_order &&
                           cp.encoding.preserve_quality &&
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
      if (cp.read_info.id_crc[i] != 0 && id_crc[i] != cp.read_info.id_crc[i]) {
        Logger::log_error("Stream " + std::to_string(i + 1) +
                          " ID digest mismatch: expected=" +
                          std::to_string(cp.read_info.id_crc[i]) +
                          " actual=" + std::to_string(id_crc[i]));
        std::cerr << "Stream " << (i + 1) << " ID digest mismatch!\n";
        mismatch = true;
      }
    }
    if (mismatch)
      throw std::runtime_error("Archive integrity audit failed!");
  }

  std::cout << "Audit successful: " << archive_label << " is valid."
            << std::endl;
}

void perform_audit_standard_bytes(const std::string &archive_contents,
                                  const std::string &archive_label) {
  decompression_archive_artifact artifact;
  artifact.files = read_all_files_from_tar_bytes(archive_contents);
  artifact.scratch_dir.clear();
  perform_audit_standard_artifact(artifact, archive_label);
}

} // namespace

void perform_audit_standard(const std::string &archive_path,
                            const std::string &temp_dir) {
  (void)temp_dir;
  decompression_archive_artifact artifact;
  artifact.files = read_all_files_from_tar_memory(archive_path);
  artifact.scratch_dir.clear();
  perform_audit_standard_artifact(artifact, archive_path);
}

void perform_audit(const std::string &archive_path,
                   const std::string &temp_dir) {
  (void)temp_dir;
  SPRING_LOG_DEBUG("Audit started for archive: " + archive_path +
                   " (in-memory)");

  const std::unordered_map<std::string, std::string> top_level_files =
      read_all_files_from_tar_memory(archive_path);
  auto manifest_it = top_level_files.find(kBundleManifestName);
  if (manifest_it != top_level_files.end()) {
    const bundle_manifest manifest =
        read_bundle_manifest_from_string(manifest_it->second);

    auto require_member =
        [&](const std::string &member_name) -> const std::string & {
      auto member_it = top_level_files.find(member_name);
      if (member_it == top_level_files.end()) {
        throw std::runtime_error(
            "Grouped archive is missing required member: " + member_name);
      }
      return member_it->second;
    };

    perform_audit_standard_bytes(require_member(manifest.read_archive_name),
                                 manifest.read_archive_name);
    if (manifest.has_r3 && manifest.read3_alias_source.empty()) {
      perform_audit_standard_bytes(require_member(manifest.read3_archive_name),
                                   manifest.read3_archive_name);
    }
    if (manifest.has_index) {
      perform_audit_standard_bytes(require_member(manifest.index_archive_name),
                                   manifest.index_archive_name);
    }

    std::cout << "Audit successful: grouped archive members are valid."
              << std::endl;
    return;
  }

  decompression_archive_artifact artifact;
  artifact.files = top_level_files;
  artifact.scratch_dir.clear();
  perform_audit_standard_artifact(artifact, archive_path);
}

void compress_standard(const std::string &temp_dir,
                       const string_list &input_paths,
                       const string_list &output_paths, const int num_thr,
                       const bool pairing_only_flag, const bool no_quality_flag,
                       const bool no_ids_flag,
                       const string_list &quality_options,
                       const int compression_level, const std::string &note,
                       const log_level /*verbosity_level*/,
                       const bool audit_flag, const std::string &r3_path,
                       const std::string &i1_path, const std::string &i2_path,
                       const std::string &assay_type,
                       const std::string &cb_source_path, uint32_t cb_len,
                       std::string *archive_bytes_output = nullptr) {
  const auto compression_start = clock_type::now();

  const compression_io_config io_config =
      resolve_compression_io(input_paths, output_paths);
  validate_compression_target(input_paths, io_config.archive_path);
  prepared_compression_inputs prepared_inputs =
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

  SPRING_LOG_INFO(
      "Analyzing first 10,000 fragments for startup properties and assay...");
  AssayDetector detector;
  const AssayDetector::StartupAnalysisResult startup_sample =
      detector.analyze_startup_sample(
          prepared_inputs.input_path_1,
          io_config.paired_end ? prepared_inputs.input_path_2 : "", r3_path,
          i1_path, i2_path, io_config.paired_end, fasta_input);

  input_detection_summary detected_input = startup_sample.input_summary;
  if (detected_input.requires_long_mode()) {
    SPRING_LOG_INFO("Startup sample indicates long-read mode; running full "
                    "input pre-scan.");
    detected_input = detect_input_properties(prepared_inputs.input_path_1,
                                             prepared_inputs.input_path_2,
                                             io_config.paired_end, fasta_input);
  }

  const bool use_crlf = detected_input.use_crlf();
  bool long_flag = detected_input.requires_long_mode();
  SPRING_LOG_DEBUG(
      "Detected maximum read length=" +
      std::to_string(detected_input.max_read_length) + ", use_crlf=" +
      std::string(use_crlf ? "true" : "false") + ", non_acgtn_symbols=" +
      std::string(detected_input.contains_non_acgtn_symbols ? "true"
                                                            : "false") +
      ", long_mode=" + std::string(long_flag ? "true" : "false"));

  if (detected_input.contains_non_acgtn_symbols) {
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
  cp.encoding.use_crlf_by_stream[0] = detected_input.use_crlf_by_stream[0];
  cp.encoding.use_crlf_by_stream[1] =
      io_config.paired_end ? detected_input.use_crlf_by_stream[1] : false;
  cp.encoding.num_reads_per_block = NUM_READS_PER_BLOCK;
  cp.encoding.num_reads_per_block_long = NUM_READS_PER_BLOCK_LONG;
  cp.encoding.num_thr = num_thr;
  cp.encoding.compression_level = compression_level;
  cp.read_info.note = note;

  std::string final_assay = assay_type;
  std::string final_confidence = "N/A";
  if (assay_type == "auto") {
    const AssayDetector::DetectionResult &res = startup_sample.assay_result;
    final_assay = res.assay;
    final_confidence = res.confidence;

    // Apply ternary guard: < 5% C or < 5% G (bisulfite conversion)
    if (final_assay == "bisulfite" || final_assay == "sc-bisulfite") {
      if (res.c_ratio < 0.05 || res.g_ratio < 0.05) {
        cp.encoding.bisulfite_ternary = true;
        cp.encoding.depleted_base = res.depleted_base;
      }
    }
    SPRING_LOG_INFO("Auto-detected assay: " + final_assay +
                    " (confidence: " + final_confidence + ")");
  }

  cp.read_info.assay = final_assay;
  cp.read_info.assay_confidence = final_confidence;
  cp.read_info.compressor_version = spring::VERSION;
  cp.encoding.cb_len = cb_len;
  cp.encoding.cb_prefix_source_external = !cb_source_path.empty();

  cp.encoding.fasta_mode = fasta_input;
  cp.read_info.input_filename_1 =
      std::filesystem::path(io_config.input_path_1).filename().string();
  if (io_config.paired_end) {
    cp.read_info.input_filename_2 =
        std::filesystem::path(io_config.input_path_2).filename().string();
  }

  SPRING_LOG_DEBUG("Archive metadata inputs: name1='" +
                   cp.read_info.input_filename_1 + "'" +
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
    SPRING_LOG_INFO("FASTA input detected; quality values will not be stored.");
  }

  if (prepared_inputs.input_1_was_gzipped ||
      prepared_inputs.input_2_was_gzipped) {
    SPRING_LOG_INFO("Detected gzipped input; streaming decompression directly "
                    "into compression without staging full plain FASTQ files.");
  }

  SPRING_LOG_DEBUG(
      "Effective encoding options: paired_end=" +
      std::string(cp.encoding.paired_end ? "true" : "false") +
      ", preserve_order=" +
      std::string(cp.encoding.preserve_order ? "true" : "false") +
      ", preserve_id=" +
      std::string(cp.encoding.preserve_id ? "true" : "false") +
      ", preserve_quality=" +
      std::string(cp.encoding.preserve_quality ? "true" : "false") +
      ", fasta_mode=" + std::string(cp.encoding.fasta_mode ? "true" : "false"));

  auto *progress_ptr = ProgressBar::GlobalInstance();
  ProgressBar dummy_progress(true);
  auto &progress = progress_ptr ? *progress_ptr : dummy_progress;

  const compression_params preprocess_seed_cp = cp;
  input_detection_summary preprocess_seed_summary = detected_input;
  const bool validate_sample_during_preprocess =
      !startup_sample.input_summary.requires_long_mode();
  preprocess_artifact preprocess_output;

  for (int preprocess_attempt = 0;; ++preprocess_attempt) {
    compression_params attempt_cp = preprocess_seed_cp;
    attempt_cp.encoding.long_flag =
        preprocess_seed_summary.requires_long_mode();
    attempt_cp.encoding.use_crlf = preprocess_seed_summary.use_crlf();
    attempt_cp.encoding.use_crlf_by_stream[0] =
        preprocess_seed_summary.use_crlf_by_stream[0];
    attempt_cp.encoding.use_crlf_by_stream[1] =
        io_config.paired_end ? preprocess_seed_summary.use_crlf_by_stream[1]
                             : false;

    try {
      run_timed_step("Preprocessing ...", "Preprocessing", [&] {
        progress.set_stage("Preprocessing", 0.0F, 0.25F);
        preprocess_output = preprocess(
            prepared_inputs.input_path_1, prepared_inputs.input_path_2,
            temp_dir, attempt_cp, fasta_input, &progress,
            validate_sample_during_preprocess ? &preprocess_seed_summary
                                              : nullptr);
      });
      cp = std::move(attempt_cp);
      long_flag = cp.encoding.long_flag;
      break;
    } catch (const preprocess_retry_exception &retry) {
      if (preprocess_attempt >= 1) {
        throw;
      }

      preprocess_seed_summary = retry.updated_summary();
      if (preprocess_seed_summary.requires_long_mode()) {
        SPRING_LOG_INFO(
            "Preprocessing found long-read properties outside the startup "
            "sample; running full input pre-scan before retry.");
      } else {
        SPRING_LOG_INFO(
            "Preprocessing found startup metadata outside the startup sample; "
            "retrying with updated properties.");
      }

      cleanup_prepared_compression_inputs(prepared_inputs, pairing_only_flag);
      recreate_compression_workdir(temp_dir);
      prepared_inputs =
          prepare_compression_inputs(io_config, temp_dir, num_thr);

      if (preprocess_seed_summary.requires_long_mode()) {
        preprocess_seed_summary = detect_input_properties(
            prepared_inputs.input_path_1, prepared_inputs.input_path_2,
            io_config.paired_end, fasta_input);
      }
    }
  }
  cleanup_prepared_compression_inputs(prepared_inputs, pairing_only_flag);
  print_temp_dir_size(temp_dir);

  std::unordered_map<std::string, std::string> archive_members =
      std::move(preprocess_output.archive_members);

  if (!long_flag) {
    reorder_encoder_artifact reorder_artifact;
    post_encode_side_stream_artifact post_encode_side_streams;
    reordered_stream_artifact reordered_streams_artifact;
    const bool needs_post_encode_side_streams =
        !preserve_order &&
        (preserve_quality || preserve_id || cp.encoding.poly_at_stripped ||
         cp.encoding.atac_adapter_stripped);
    if (needs_post_encode_side_streams) {
      post_encode_side_streams =
          std::move(preprocess_output.post_encode_side_streams);
    }

    // Run overlap-based reordering for all assays.
    run_timed_step("Reordering ...", "Reordering", [&] {
      progress.set_stage("Reordering", 0.25F, 0.50F);
      reorder_artifact =
          call_reorder(temp_dir, preprocess_output.reorder_inputs, cp);
    });

    print_temp_dir_size(temp_dir, "temp_dir size");

    run_timed_step("Encoding ...", "Encoding", [&] {
      progress.set_stage("Encoding", 0.50F, 0.85F);
      reordered_streams_artifact = call_encoder(temp_dir, reorder_artifact, cp);
    });

    print_temp_dir_size(temp_dir);

    if (needs_post_encode_side_streams) {
      run_timed_step("Reordering and compressing quality and/or ids ...",
                     "Reordering and compressing quality and/or ids", [&] {
                       merge_archive_members(
                           archive_members,
                           reorder_compress_quality_id(
                               temp_dir, post_encode_side_streams,
                               reordered_streams_artifact.read_order_entries,
                               cp));
                     });
      print_temp_dir_size(temp_dir);
    }

    if (!preserve_order && io_config.paired_end) {
      run_timed_step("Encoding pairing information ...",
                     "Encoding pairing information", [&] {
                       pe_encode(reordered_streams_artifact.read_order_entries,
                                 cp);
                     });
      print_temp_dir_size(temp_dir);
    }

    run_timed_step("Reordering and compressing streams ...",
                   "Reordering and compressing streams", [&] {
                     progress.set_stage("Compressing streams", 0.85F, 0.95F);
                     merge_archive_members(
                         archive_members,
                         reordered_streams_artifact.archive_members);
                     merge_archive_members(
                         archive_members,
                         reorder_compress_streams(
                             temp_dir, cp, reordered_streams_artifact,
                             &reordered_streams_artifact.read_order_entries));
                   });
    print_temp_dir_size(temp_dir);
  }

  archive_members["cp.bin"] = serialize_compression_params(cp);

  print_compressed_stream_sizes(archive_members);

  const std::vector<tar_archive_source> archive_sources =
      build_archive_sources(archive_members);

  run_timed_step("Creating tar archive ...", "Tar archive", [&] {
    progress.set_stage("Creating archive", 0.95F, 1.0F);
    if (archive_bytes_output != nullptr) {
      *archive_bytes_output =
          create_tar_archive_from_sources_bytes(archive_sources);
    } else {
      create_tar_archive_from_sources(io_config.archive_path, archive_sources);
    }
  });
  if (archive_bytes_output != nullptr) {
    SPRING_LOG_DEBUG("Archive created in memory: bytes=" +
                     std::to_string(archive_bytes_output->size()));
  } else {
    SPRING_LOG_DEBUG("Archive created at: " + io_config.archive_path);
  }

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
    std::cout << "\n";
    if (archive_bytes_output != nullptr) {
      std::cout << "Total size: " << std::setw(12)
                << archive_bytes_output->size() << " bytes\n";
    } else {
      fs::path archive_file_path{io_config.archive_path};
      std::cout << "Total size: " << std::setw(12)
                << fs::file_size(archive_file_path) << " bytes\n";
    }
  }

  if (audit_flag && archive_bytes_output == nullptr) {
    SPRING_LOG_DEBUG("Running post-compression audit.");
    perform_audit_standard(io_config.archive_path, temp_dir);
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
              const std::string &i2_path, const std::string &assay_type,
              const std::string &cb_source_path, uint32_t cb_len) {
  Logger::set_level(verbosity_level);
  ProgressBar progress(verbosity_level == log_level::quiet);
  ProgressBar::SetGlobalInstance(&progress);
  omp_set_dynamic(0);

  SPRING_LOG_INFO("Starting compression...");
  SPRING_LOG_DEBUG(
      "Compression request: temp_dir=" + temp_dir + ", num_threads=" +
      std::to_string(num_thr) + ", level=" + std::to_string(compression_level) +
      ", strip_order=" + std::string(pairing_only_flag ? "true" : "false") +
      ", strip_quality=" + std::string(no_quality_flag ? "true" : "false") +
      ", strip_ids=" + std::string(no_ids_flag ? "true" : "false") +
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
    validate_compression_target(input_paths, output_archive_path);
    const std::string read_archive_name = "reads_group.sp";
    const std::string read3_archive_name = "read3_group.sp";
    const std::string index_archive_name = "index_group.sp";

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

    std::string read_archive_bytes;
    std::string read3_archive_bytes;
    std::string index_archive_bytes;

    // Compress R1/R2 as a regular SPRING archive.
    // I1 path is passed so CB extraction can use it during preprocessing.
    compress_standard(temp_dir, read_inputs, {}, num_thr, pairing_only_flag,
                      no_quality_flag, no_ids_flag, quality_options,
                      compression_level, note, verbosity_level, false, "",
                      i1_path, "", assay_type, i1_path, cb_len,
                      &read_archive_bytes);

    const std::string grouped_assay =
        (assay_type == "auto") ? assay_from_archive_metadata_bytes(
                                     read_archive_bytes, read_archive_name)
                               : assay_type;

    std::string read3_alias_source;
    if (has_r3) {
      if (paths_refer_to_same_file(r3_path, input_paths[0])) {
        read3_alias_source = "R1";
      } else if (paths_refer_to_same_file(r3_path, input_paths[1])) {
        read3_alias_source = "R2";
      } else {
        compress_standard(temp_dir, read3_inputs, {}, num_thr,
                          pairing_only_flag, no_quality_flag, no_ids_flag,
                          quality_options, compression_level,
                          note.empty() ? std::string("read3-group")
                                       : (note + " | read3-group"),
                          verbosity_level, false, "", "", "", grouped_assay, "",
                          cb_len, &read3_archive_bytes);
      }
    }

    if (has_i1) {
      compress_standard(
          temp_dir, index_inputs, {}, num_thr, pairing_only_flag,
          no_quality_flag, no_ids_flag, quality_options, compression_level,
          note.empty() ? std::string("index-group") : (note + " | index-group"),
          verbosity_level, false, "", "", "", grouped_assay, "", cb_len,
          &index_archive_bytes);
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
    std::vector<tar_archive_source> bundle_sources;
    bundle_sources.push_back({.archive_path = read_archive_name,
                              .disk_path = std::string(),
                              .contents = read_archive_bytes,
                              .from_memory = true});
    if (has_r3 && read3_alias_source.empty()) {
      bundle_sources.push_back({.archive_path = read3_archive_name,
                                .disk_path = std::string(),
                                .contents = read3_archive_bytes,
                                .from_memory = true});
    }
    if (has_i1) {
      bundle_sources.push_back({.archive_path = index_archive_name,
                                .disk_path = std::string(),
                                .contents = index_archive_bytes,
                                .from_memory = true});
    }
    bundle_sources.push_back({.archive_path = kBundleManifestName,
                              .disk_path = std::string(),
                              .contents = serialize_bundle_manifest(manifest),
                              .from_memory = true});

    run_timed_step("Creating grouped bundle archive ...", "Tar archive", [&] {
      progress.set_stage("Creating archive", 0.95F, 1.0F);
      create_tar_archive_from_sources(output_archive_path, bundle_sources);
    });
    SPRING_LOG_DEBUG("Grouped archive created at: " + output_archive_path);

    if (audit_flag) {
      SPRING_LOG_DEBUG("Running post-compression audit for grouped archive.");
      perform_audit(output_archive_path, temp_dir);
    }

    ProgressBar::SetGlobalInstance(nullptr);
    return;
  }

  compress_standard(temp_dir, input_paths, output_paths, num_thr,
                    pairing_only_flag, no_quality_flag, no_ids_flag,
                    quality_options, compression_level, note, verbosity_level,
                    audit_flag, r3_path, i1_path, i2_path, assay_type,
                    cb_source_path, cb_len);
}

void decompress_archive_artifact(const decompression_archive_artifact &artifact,
                                 const std::vector<std::string> &input_paths,
                                 const std::vector<std::string> &output_paths,
                                 const int num_thr, const bool unzip_flag) {
  const auto decompression_start = clock_type::now();
  auto *progress_ptr = ProgressBar::GlobalInstance();
  ProgressBar dummy_progress(true);
  auto &progress = progress_ptr ? *progress_ptr : dummy_progress;
  compression_params cp{};

  std::istringstream compression_params_input(artifact.require("cp.bin"),
                                              std::ios::binary);
  read_compression_params(compression_params_input, cp);
  if (!compression_params_input.good())
    throw std::runtime_error("Can't read compression parameters.");
  // IMPORTANT: cp.encoding.num_thr is the *encoding* thread count and controls
  // how many packed sequence chunks exist in the archive. DO NOT override it
  // with the user-specified decoding parallelism, or the decompressor will
  // attempt to read the wrong number of chunks and get out-of-range positions.
  // Use a separate variable for decoding parallelism.
  const int decoding_num_thr = (num_thr > 0) ? num_thr : cp.encoding.num_thr;

  bool paired_end = cp.encoding.paired_end;
  SPRING_LOG_DEBUG(
      "Archive metadata: paired_end=" +
      std::string(cp.encoding.paired_end ? "true" : "false") +
      ", long_mode=" + std::string(cp.encoding.long_flag ? "true" : "false") +
      ", preserve_order=" +
      std::string(cp.encoding.preserve_order ? "true" : "false") +
      ", preserve_quality=" +
      std::string(cp.encoding.preserve_quality ? "true" : "false") +
      ", preserve_id=" +
      std::string(cp.encoding.preserve_id ? "true" : "false") +
      ", fasta_mode=" + std::string(cp.encoding.fasta_mode ? "true" : "false"));

  const decompression_io_config io_config =
      resolve_decompression_io(input_paths, output_paths, paired_end);
  validate_output_targets(
      io_config.archive_path,
      paired_end ? std::vector<std::string>{io_config.output_path_1,
                                            io_config.output_path_2}
                 : std::vector<std::string>{io_config.output_path_1});

  auto has_compressed_suffix = [](const std::string &path) {
    return path.ends_with(".gz");
  };

  bool should_gzip[2] = {false, false};
  bool should_bgzf[2] = {false, false};
  int compression_levels[2] = {cp.encoding.compression_level,
                               cp.encoding.compression_level};
  for (int i = 0; i < (paired_end ? 2 : 1); i++) {
    const std::string &path =
        (i == 0) ? io_config.output_path_1 : io_config.output_path_2;
    if (!unzip_flag && has_compressed_suffix(path)) {
      should_gzip[i] = true;
      should_bgzf[i] =
          (i == 0) ? cp.gzip.streams[0].is_bgzf : cp.gzip.streams[1].is_bgzf;
      compression_levels[i] = gzip_output_compression_level(
          (i == 0) ? cp.gzip.streams[0] : cp.gzip.streams[1],
          cp.encoding.compression_level);
    }
  }

  const bool write_enabled[2] = {true, true};
  std::unique_ptr<DecompressionSink> sink =
      std::make_unique<FileDecompressionSink>(
          io_config.output_path_1, io_config.output_path_2, cp,
          compression_levels, should_gzip, should_bgzf, write_enabled);

  if (cp.encoding.long_flag) {
    decompress_long(artifact, *sink, cp, decoding_num_thr);
  } else {
    decompress_short(artifact, *sink, cp, decoding_num_thr);
  }

  run_timed_step("Verifying integrity ...", "Integrity check", [&] {
    const bool is_lossless =
        cp.encoding.preserve_order && cp.encoding.preserve_quality &&
        cp.encoding.preserve_id && !cp.quality.qvz_flag &&
        !cp.quality.ill_bin_flag && !cp.quality.bin_thr_flag;

    if (is_lossless) {
      uint32_t seq_crc[2], qual_crc[2], id_crc[2];
      sink->get_digests(seq_crc, qual_crc, id_crc);
      bool mismatch = false;
      for (int i = 0; i < (cp.encoding.paired_end ? 2 : 1); ++i) {
        if (cp.read_info.sequence_crc[i] != 0 &&
            seq_crc[i] != cp.read_info.sequence_crc[i]) {
          Logger::log_error("Stream " + std::to_string(i + 1) +
                            " sequence digest mismatch.");
          mismatch = true;
        }
        if (cp.read_info.quality_crc[i] != 0 &&
            qual_crc[i] != cp.read_info.quality_crc[i]) {
          Logger::log_error("Stream " + std::to_string(i + 1) +
                            " quality digest mismatch.");
          mismatch = true;
        }
        if (cp.read_info.id_crc[i] != 0 &&
            id_crc[i] != cp.read_info.id_crc[i]) {
          Logger::log_error("Stream " + std::to_string(i + 1) +
                            " ID digest mismatch.");
          mismatch = true;
        }
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
}

void decompress_standard(const std::string &temp_dir,
                         const std::vector<std::string> &input_paths,
                         const std::vector<std::string> &output_paths,
                         const int num_thr, const int /*compression_level*/,
                         const log_level /*verbosity_level*/,
                         const bool unzip_flag, const bool untar_first = true) {
  (void)untar_first;
  (void)temp_dir;
  decompression_archive_artifact artifact;
  artifact.files = read_all_files_from_tar_memory(input_paths[0]);
  artifact.scratch_dir.clear();
  decompress_archive_artifact(artifact, input_paths, output_paths, num_thr,
                              unzip_flag);
}

void decompress_standard_from_memory(
    const std::string &temp_dir, const std::string &archive_contents,
    const std::string &archive_label,
    const std::vector<std::string> &output_paths, const int num_thr,
    const int compression_level, const log_level verbosity_level,
    const bool unzip_flag) {
  (void)temp_dir;
  (void)compression_level;
  (void)verbosity_level;
  decompression_archive_artifact artifact;
  artifact.files = read_all_files_from_tar_bytes(archive_contents);
  artifact.scratch_dir.clear();
  decompress_archive_artifact(artifact, {archive_label}, output_paths, num_thr,
                              unzip_flag);
}

void materialize_aliased_group_output_from_memory(
    const std::string &temp_dir, const std::string &read_archive_contents,
    const std::string &read_archive_label, const std::string &alias_source,
    const std::string &alias_output_path, const int num_thr,
    const int compression_level, const log_level verbosity_level,
    const bool unzip_flag) {
  (void)temp_dir;
  (void)compression_level;
  (void)verbosity_level;

  decompression_archive_artifact artifact;
  artifact.files = read_all_files_from_tar_bytes(read_archive_contents);
  artifact.scratch_dir.clear();

  compression_params cp{};
  std::istringstream compression_params_input(artifact.require("cp.bin"),
                                              std::ios::binary);
  read_compression_params(compression_params_input, cp);
  if (!compression_params_input.good()) {
    throw std::runtime_error("Can't read compression parameters.");
  }

  const int decoding_num_thr = (num_thr > 0) ? num_thr : cp.encoding.num_thr;
  const int selected_stream = (alias_source == "R2") ? 1 : 0;
  bool should_gzip[2] = {false, false};
  bool should_bgzf[2] = {false, false};
  bool write_enabled[2] = {false, false};
  int compression_levels[2] = {cp.encoding.compression_level,
                               cp.encoding.compression_level};
  write_enabled[selected_stream] = true;
  if (!unzip_flag && alias_output_path.ends_with(".gz")) {
    should_gzip[selected_stream] = true;
    should_bgzf[selected_stream] = cp.gzip.streams[selected_stream].is_bgzf;
    compression_levels[selected_stream] = gzip_output_compression_level(
        cp.gzip.streams[selected_stream], cp.encoding.compression_level);
  }

  const std::string output_path_1 =
      (selected_stream == 0) ? alias_output_path : std::string();
  const std::string output_path_2 =
      (selected_stream == 1) ? alias_output_path : std::string();
  FileDecompressionSink sink(output_path_1, output_path_2, cp,
                             compression_levels, should_gzip, should_bgzf,
                             write_enabled);
  if (cp.encoding.long_flag) {
    decompress_long(artifact, sink, cp, decoding_num_thr);
  } else {
    decompress_short(artifact, sink, cp, decoding_num_thr);
  }

  const bool is_lossless = cp.encoding.preserve_order &&
                           cp.encoding.preserve_quality &&
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
                          " sequence digest mismatch.");
        mismatch = true;
      }
      if (cp.read_info.quality_crc[i] != 0 &&
          qual_crc[i] != cp.read_info.quality_crc[i]) {
        Logger::log_error("Stream " + std::to_string(i + 1) +
                          " quality digest mismatch.");
        mismatch = true;
      }
      if (cp.read_info.id_crc[i] != 0 && id_crc[i] != cp.read_info.id_crc[i]) {
        Logger::log_error("Stream " + std::to_string(i + 1) +
                          " ID digest mismatch.");
        mismatch = true;
      }
    }
    if (mismatch) {
      throw std::runtime_error(
          "ARCHIVE INTEGRITY CHECK FAILED: Reconstructed data does not match "
          "original digests. The archive may be corrupted.");
    }
  }
}

void decompress(const std::string &temp_dir,
                const std::vector<std::string> &input_paths,
                const std::vector<std::string> &output_paths, const int num_thr,
                const int compression_level, const log_level verbosity_level,
                const bool unzip_flag) {
  Logger::set_level(verbosity_level);
  ProgressBar progress(verbosity_level == log_level::quiet);
  ProgressBar::SetGlobalInstance(&progress);
  omp_set_dynamic(0);

  SPRING_LOG_INFO("Starting decompression...");
  SPRING_LOG_DEBUG("Decompression request: temp_dir=" + temp_dir +
                   ", unzip=" + std::string(unzip_flag ? "true" : "false"));
  compression_params cp{};

  if (input_paths.size() != 1)
    throw std::runtime_error("Number of input files not equal to 1");

  const auto manifest_contents =
      read_files_from_tar_memory(input_paths[0], {kBundleManifestName});
  if (manifest_contents.contains(kBundleManifestName)) {
    const bundle_manifest manifest = read_bundle_manifest_from_string(
        manifest_contents.at(kBundleManifestName));
    SPRING_LOG_INFO("Detected grouped bundle archive (reads + optional read3 + "
                    "optional index reads).");

    std::vector<std::string> resolved_outputs;
    if (output_paths.empty()) {
      resolved_outputs = build_default_grouped_output_paths(manifest);
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
    validate_output_targets(input_paths[0], resolved_outputs);

    std::vector<std::string> member_names = {manifest.read_archive_name};
    if (manifest.has_r3 && manifest.read3_alias_source.empty()) {
      member_names.push_back(manifest.read3_archive_name);
    }
    if (manifest.has_index) {
      member_names.push_back(manifest.index_archive_name);
    }

    std::unordered_map<std::string, std::string> grouped_archives;
    run_timed_step(
        "Loading grouped bundle archive ...", "Loading grouped archive", [&] {
          progress.set_stage("Loading grouped archive", 0.0F, 0.10F);
          grouped_archives =
              read_files_from_tar_memory(input_paths[0], member_names);
        });

    auto require_group_member =
        [&](const std::string &member_name) -> const std::string & {
      auto it = grouped_archives.find(member_name);
      if (it == grouped_archives.end()) {
        throw std::runtime_error("Grouped archive is missing member: " +
                                 member_name);
      }
      return it->second;
    };

    const std::string &read_archive_contents =
        require_group_member(manifest.read_archive_name);

    decompress_standard_from_memory(
        temp_dir, read_archive_contents, manifest.read_archive_name,
        {resolved_outputs[0], resolved_outputs[1]}, num_thr, compression_level,
        verbosity_level, unzip_flag);

    size_t next_output_index = 2;
    if (manifest.has_r3) {
      if (!manifest.read3_alias_source.empty()) {
        materialize_aliased_group_output_from_memory(
            temp_dir, read_archive_contents, manifest.read_archive_name,
            manifest.read3_alias_source, resolved_outputs[next_output_index],
            num_thr, compression_level, verbosity_level, unzip_flag);
      } else {
        decompress_standard_from_memory(
            temp_dir, require_group_member(manifest.read3_archive_name),
            manifest.read3_archive_name, {resolved_outputs[next_output_index]},
            num_thr, compression_level, verbosity_level, unzip_flag);
      }
      next_output_index++;
    }

    if (manifest.has_index) {
      const std::string &index_archive_contents =
          require_group_member(manifest.index_archive_name);
      if (manifest.has_i2) {
        decompress_standard_from_memory(
            temp_dir, index_archive_contents, manifest.index_archive_name,
            {resolved_outputs[next_output_index],
             resolved_outputs[next_output_index + 1]},
            num_thr, compression_level, verbosity_level, unzip_flag);
      } else {
        decompress_standard_from_memory(
            temp_dir, index_archive_contents, manifest.index_archive_name,
            {resolved_outputs[next_output_index]}, num_thr, compression_level,
            verbosity_level, unzip_flag);
      }
    }

    ProgressBar::SetGlobalInstance(nullptr);
    return;
  }

  run_timed_step("Loading archive into memory ...", "Loading archive", [&] {
    progress.set_stage("Loading archive", 0.0F, 0.10F);
    decompress_standard(temp_dir, input_paths, output_paths, num_thr,
                        compression_level, verbosity_level, unzip_flag, false);
  });
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
