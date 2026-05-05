#pragma once

#include <fstream>
#include <string>

#include "../support/doctest.h"

#ifndef SPRING2_EXECUTABLE
#define SPRING2_EXECUTABLE "spring2"
#endif

namespace integration_test_support {

std::string read_file_binary(const std::string &path);
std::string read_gzip_file_binary(const std::string &path);

void write_fastq_record(std::ofstream &output, const std::string &id,
                        const std::string &sequence, const std::string &quality,
                        bool quality_header_has_id, bool use_crlf);
void create_custom_fastq(const std::string &path, int num_records,
                         bool quality_header_has_id, bool use_crlf,
                         int read_len = 80);
void create_late_long_fastq(const std::string &path, int short_records,
                            int total_records, int short_len, int long_len);
void create_delayed_crlf_fastq(const std::string &path, int total_records,
                               int lf_records, int read_len = 80);
void create_custom_paired_fastqs(const std::string &r1_path,
                                 const std::string &r2_path, int num_records,
                                 bool r1_quality_header_has_id,
                                 bool r2_quality_header_has_id,
                                 bool r1_use_crlf, bool r2_use_crlf);
void create_gzip_copy(const std::string &input_path,
                      const std::string &output_path, int level);
void create_dummy_fastq(const std::string &path, int num_records);
void create_atac_like_fastq(const std::string &path, int num_records);
void create_sparse_atac_like_fastq(const std::string &path, int num_records);
void create_grouped_sc_rna_like_fastqs(const std::string &r1_path,
                                       const std::string &r2_path,
                                       const std::string &i1_path,
                                       const std::string &i2_path,
                                       int num_records);
std::string read_manifest_value(const std::string &manifest_path,
                                const std::string &key);
void create_tar_with_entry(const std::string &archive_path,
                           const std::string &entry_path,
                           const std::string &contents);
void replace_exact_in_file(const std::string &path, const std::string &from,
                           const std::string &to);

struct ScopedCurrentPath {
  explicit ScopedCurrentPath(const std::string &path);
  ~ScopedCurrentPath();

  std::string original;
};

} // namespace integration_test_support