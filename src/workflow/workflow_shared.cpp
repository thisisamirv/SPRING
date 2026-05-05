// Implements shared workflow helpers used by the top-level compression,
// decompression, and audit entrypoints.

#include "workflow_internal.h"

#include "io_utils.h"
#include "parse_utils.h"

#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace spring {

std::string default_archive_name_from_input(const std::string &input_path) {
  std::filesystem::path p = std::filesystem::path(input_path).filename();
  bool changed = true;
  while (changed) {
    changed = false;
    const std::string ext = p.extension().string();
    for (const std::string &value : {".gz", ".fastq", ".fq", ".fasta", ".fa",
                                     ".FASTQ", ".FQ", ".FASTA", ".FA"}) {
      if (ext == value) {
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

  std::vector<output_role> requested_outputs = {{manifest.r1_name, "R1"},
                                                {manifest.r2_name, "R2"}};
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

prepared_compression_inputs
prepare_compression_inputs(const compression_io_config &io_config,
                           const int /*num_thr*/) {
  SPRING_LOG_DEBUG("Preparing compression inputs for direct streaming.");
  return {.input_path_1 = io_config.input_path_1,
          .input_path_2 = io_config.input_path_2,
          .input_1_was_gzipped = is_gzip_input_path(io_config.input_path_1),
          .input_2_was_gzipped = io_config.paired_end &&
                                 is_gzip_input_path(io_config.input_path_2)};
}

void cleanup_prepared_compression_inputs(
    const prepared_compression_inputs & /*prepared_inputs*/,
    bool /*pairing_only_flag*/) {}

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
    io_config.archive_path = default_archive_name_from_input(input_paths[0]);
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

void print_compressed_stream_sizes(
    const std::unordered_map<std::string, std::string> &archive_members) {
  uint64_t size_read = 0;
  uint64_t size_quality = 0;
  uint64_t size_id = 0;
  for (const auto &[name, contents] : archive_members) {
    if (name == "cp.bin") {
      continue;
    }
    if (name.starts_with("quality") || name.starts_with("qv")) {
      size_quality += contents.size();
    } else if (name.starts_with("id")) {
      size_id += contents.size();
    } else {
      size_read += contents.size();
    }
  }

  SPRING_LOG_INFO("Compressed read bytes: " + std::to_string(size_read));
  if (size_quality != 0) {
    SPRING_LOG_INFO("Compressed quality bytes: " +
                    std::to_string(size_quality));
  }
  if (size_id != 0) {
    SPRING_LOG_INFO("Compressed ID bytes: " + std::to_string(size_id));
  }
}

void merge_archive_members(
    std::unordered_map<std::string, std::string> &archive_members,
    std::unordered_map<std::string, std::string> new_members) {
  for (auto &entry : new_members) {
    archive_members.insert_or_assign(entry.first, std::move(entry.second));
  }
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
  for (const auto &[archive_path, contents] : archive_members) {
    sources.push_back({.archive_path = archive_path,
                       .disk_path = std::string(),
                       .contents = contents,
                       .from_memory = true});
  }
  return sources;
}

decompression_io_config
resolve_decompression_io(const string_list &input_paths,
                         const string_list &output_paths,
                         const bool paired_end) {
  if (input_paths.size() != 1) {
    throw std::runtime_error("Number of input files not equal to 1");
  }

  decompression_io_config io_config;
  io_config.archive_path = input_paths[0];

  std::filesystem::path base = std::filesystem::path(input_paths[0]).filename();
  if (base.extension() == ".sp") {
    base.replace_extension("");
  }
  const std::string base_name = base.string();

  if (output_paths.empty()) {
    if (paired_end) {
      io_config.output_path_1 = base_name + ".1";
      io_config.output_path_2 = base_name + ".2";
    } else {
      io_config.output_path_1 = base_name + ".fastq";
    }
  } else if (output_paths.size() == 1) {
    if (paired_end) {
      io_config.output_path_1 = output_paths[0] + ".1";
      io_config.output_path_2 = output_paths[0] + ".2";
    } else {
      io_config.output_path_1 = output_paths[0];
    }
  } else if (paired_end && output_paths.size() == 2) {
    io_config.output_path_1 = output_paths[0];
    io_config.output_path_2 = output_paths[1];
  } else {
    throw std::runtime_error("Invalid number of decompression output files.");
  }

  SPRING_LOG_DEBUG(
      "Resolved decompression I/O: archive=" + io_config.archive_path +
      ", output1=" + io_config.output_path_1 +
      (paired_end ? (", output2=" + io_config.output_path_2) : std::string()));
  return io_config;
}

} // namespace spring