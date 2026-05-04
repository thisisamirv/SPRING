// Implements the top-level compression workflows, including grouped bundle
// orchestration on top of the shared workflow helpers.

#include "workflow_internal.h"

#include "assay_bisulfite.h"
#include "assay_detector.h"
#include "assay_sc_bisulfite.h"
#include "input_preparation.h"
#include "io_utils.h"
#include "paired_end_mate_ordering.h"
#include "progress.h"
#include "quality_id_reordering.h"
#include "stream_reordering.h"
#include "template_dispatch.h"
#include "version.h"

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <omp.h>

namespace spring {

namespace {

void compress_standard(const string_list &input_paths,
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
      prepare_compression_inputs(io_config, num_thr);
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

    apply_bisulfite_auto_config(cp, res);
    apply_sc_bisulfite_auto_config(cp, res);
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

  extract_gzip_detailed_info(
      io_config.input_path_1, cp.gzip.streams[0].was_gzipped,
      cp.gzip.streams[0].flg, cp.gzip.streams[0].mtime, cp.gzip.streams[0].xfl,
      cp.gzip.streams[0].os, cp.gzip.streams[0].name,
      cp.gzip.streams[0].is_bgzf, cp.gzip.streams[0].bgzf_block_size,
      cp.gzip.streams[0].uncompressed_size, cp.gzip.streams[0].compressed_size,
      cp.gzip.streams[0].member_count);
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
            attempt_cp, fasta_input, &progress,
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
      prepared_inputs = prepare_compression_inputs(io_config, num_thr);

      if (preprocess_seed_summary.requires_long_mode()) {
        preprocess_seed_summary = detect_input_properties(
            prepared_inputs.input_path_1, prepared_inputs.input_path_2,
            io_config.paired_end, fasta_input);
      }
    }
  }
  cleanup_prepared_compression_inputs(prepared_inputs, pairing_only_flag);

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

    run_timed_step("Reordering ...", "Reordering", [&] {
      progress.set_stage("Reordering", 0.25F, 0.50F);
      reorder_artifact = call_reorder(preprocess_output.reorder_inputs, cp);
    });

    run_timed_step("Encoding ...", "Encoding", [&] {
      progress.set_stage("Encoding", 0.50F, 0.85F);
      reordered_streams_artifact = call_encoder(reorder_artifact, cp);
    });

    if (needs_post_encode_side_streams) {
      run_timed_step("Reordering and compressing quality and/or ids ...",
                     "Reordering and compressing quality and/or ids", [&] {
                       merge_archive_members(
                           archive_members,
                           reorder_compress_quality_id(
                               post_encode_side_streams,
                               reordered_streams_artifact.read_order_entries,
                               cp));
                     });
    }

    if (!preserve_order && io_config.paired_end) {
      run_timed_step("Encoding pairing information ...",
                     "Encoding pairing information", [&] {
                       pe_encode(reordered_streams_artifact.read_order_entries,
                                 cp);
                     });
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
                             cp, reordered_streams_artifact,
                             &reordered_streams_artifact.read_order_entries));
                   });
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
    perform_audit_standard(io_config.archive_path);
  }
}

} // namespace

void compress(const std::vector<std::string> &input_paths,
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
      "Compression request: num_threads=" + std::to_string(num_thr) +
      ", level=" + std::to_string(compression_level) +
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

    compress_standard(read_inputs, {}, num_thr, pairing_only_flag,
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
        compress_standard(read3_inputs, {}, num_thr, pairing_only_flag,
                          no_quality_flag, no_ids_flag, quality_options,
                          compression_level,
                          note.empty() ? std::string("read3-group")
                                       : (note + " | read3-group"),
                          verbosity_level, false, "", "", "", grouped_assay, "",
                          cb_len, &read3_archive_bytes);
      }
    }

    if (has_i1) {
      compress_standard(
          index_inputs, {}, num_thr, pairing_only_flag, no_quality_flag,
          no_ids_flag, quality_options, compression_level,
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
      perform_audit(output_archive_path);
    }

    ProgressBar::SetGlobalInstance(nullptr);
    return;
  }

  compress_standard(input_paths, output_paths, num_thr, pairing_only_flag,
                    no_quality_flag, no_ids_flag, quality_options,
                    compression_level, note, verbosity_level, audit_flag,
                    r3_path, i1_path, i2_path, assay_type, cb_source_path,
                    cb_len);
}

} // namespace spring