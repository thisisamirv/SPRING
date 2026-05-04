// Declares internal workflow helpers shared by the top-level compression,
// decompression, and audit entrypoint files.

#ifndef SPRING_WORKFLOW_INTERNAL_H_
#define SPRING_WORKFLOW_INTERNAL_H_

#include "archive_record_reconstruction.h"
#include "common/bundle_manifest.h"
#include "fs_utils.h"
#include "params.h"
#include "workflow_api.h"


#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace spring {

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

std::string default_archive_name_from_input(const std::string &input_path);
bool paths_refer_to_same_file(const std::string &left,
                              const std::string &right);
std::string normalized_path_key(const std::string &path);
void validate_output_targets(const std::string &archive_path,
                             const std::vector<std::string> &output_paths);
void validate_compression_target(const std::vector<std::string> &input_paths,
                                 const std::string &archive_path);
std::string assay_from_archive_metadata_bytes(const std::string &archive_bytes,
                                              const std::string &archive_label);
int gzip_output_compression_level(
    const compression_params::GzipMetadata::Stream &stream, int default_level);
std::string append_group_role_suffix(const std::string &path,
                                     const std::string &suffix);
std::vector<std::string>
build_default_grouped_output_paths(const bundle_manifest &manifest);
void print_step_summary(const char *step_name,
                        const clock_type::time_point &step_start,
                        const clock_type::time_point &step_end);

template <typename Func>
void run_timed_step(const char *start_message, const char *step_name,
                    Func &&step) {
  SPRING_LOG_INFO(start_message);
  const auto step_start = clock_type::now();
  std::forward<Func>(step)();
  const auto step_end = clock_type::now();
  print_step_summary(step_name, step_start, step_end);
}

std::string to_ascii_lowercase(std::string value);
bool is_gzip_input_path(const std::string &input_path);
std::string strip_gzip_suffix(const std::string &input_path);
prepared_compression_inputs
prepare_compression_inputs(const compression_io_config &io_config, int num_thr);
void cleanup_prepared_compression_inputs(
    const prepared_compression_inputs &prepared_inputs, bool pairing_only_flag);
bool is_fastq_extension(const std::string &path);
bool is_fasta_extension(const std::string &path);
input_record_format
detect_input_format_from_extension(const std::string &input_path,
                                   bool &detected);
input_record_format
detect_input_format_from_content(const std::string &input_path, bool &detected);
const char *input_format_name(input_record_format format);
input_record_format detect_input_format(const std::string &input_path);
compression_io_config resolve_compression_io(const string_list &input_paths,
                                             const string_list &output_paths);
void configure_quality_options(compression_params &compression_params,
                               const string_list &quality_options);
void print_compressed_stream_sizes(
    const std::unordered_map<std::string, std::string> &archive_members);
void merge_archive_members(
    std::unordered_map<std::string, std::string> &archive_members,
    std::unordered_map<std::string, std::string> new_members);
std::string serialize_compression_params(const compression_params &cp);
std::vector<tar_archive_source> build_archive_sources(
    const std::unordered_map<std::string, std::string> &archive_members);
decompression_io_config
resolve_decompression_io(const string_list &input_paths,
                         const string_list &output_paths, bool paired_end);

void perform_audit_standard(const std::string &archive_path);

} // namespace spring

#endif // SPRING_WORKFLOW_INTERNAL_H_