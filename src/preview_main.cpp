// Implements the standalone spring2-preview utility for extracting and
// displaying archive metadata, original filenames, and format notes without
// decompression.

#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>

#include "decompress.h"
#include "fs_utils.h"
#include "params.h"
#include "spring.h"
#include "version.h"

namespace spring {

namespace {

constexpr const char *kBundleManifestName = "bundle.meta";
constexpr const char *kBundleVersion = "SPRING2_BUNDLE_V1";

std::unordered_map<std::string, std::string>
read_key_value_string(const std::string &content) {
  std::unordered_map<std::string, std::string> kv;
  std::istringstream input(content);
  std::string line;
  while (std::getline(input, line)) {
    const size_t sep = line.find('=');
    if (sep == std::string::npos)
      continue;
    kv[line.substr(0, sep)] = line.substr(sep + 1);
  }
  return kv;
}

} // namespace

void perform_verification(const std::string &archive_path,
                          const std::string &temp_dir, compression_params &cp) {
  (void)cp; // Metadata already extracted in preview(), perform_audit will
            // re-extract what it needs
  perform_audit(archive_path, temp_dir);
}

void preview_single(const std::string &archive_path, bool audit_only) {
  if (audit_only) {
    const std::string temp_dir = "tmp_preview_" + random_string(10);
    std::filesystem::create_directories(temp_dir);
    try {
      compression_params cp;
      perform_verification(archive_path, temp_dir, cp);
    } catch (const std::exception &e) {
      std::error_code ec;
      std::filesystem::remove_all(temp_dir, ec);
      if (ec) {
        std::cerr << "Warning: Could not completely remove temp dir: "
                  << ec.message() << "\n";
      }
      throw;
    }
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    if (ec) {
      std::cerr << "Warning: Could not completely remove temp dir: "
                << ec.message() << "\n";
    }
    return;
  }

  // Fast path: Read metadata directly from the tar archive into memory without
  // extraction
  auto contents =
      read_files_from_tar_memory(archive_path, {kBundleManifestName, "cp.bin"});

  if (!contents.contains("cp.bin")) {
    throw std::runtime_error("Could not find cp.bin in the archive.");
  }

  compression_params cp{};
  std::istringstream in(contents["cp.bin"], std::ios::binary);
  read_compression_params(in, cp);

  std::cout << "SPRING2 Archive Metadata Preview:\n";
  std::cout << "--------------------------------\n";
  std::cout << "Original Input 1:  " << cp.read_info.input_filename_1 << "\n";
  if (cp.encoding.paired_end) {
    std::cout << "Original Input 2:  " << cp.read_info.input_filename_2 << "\n";
  }
  uint64_t archive_size = std::filesystem::file_size(archive_path);
  if (!cp.read_info.note.empty()) {
    std::cout << "Note:              " << cp.read_info.note << "\n";
  }
  std::cout << "Assay Type:        "
            << (!cp.read_info.assay.empty() ? cp.read_info.assay : "auto");
  if (!cp.read_info.assay_confidence.empty() &&
      cp.read_info.assay_confidence != "N/A") {
    std::cout << " (" << cp.read_info.assay_confidence << ")";
  }
  std::cout << "\n";
  if (cp.encoding.cb_prefix_stripped) {
    std::cout << "CB Prefix:         Extracted (" << cp.encoding.cb_prefix_len
              << " bp from R1 single-cell prefix)\n";
  }
  if (cp.encoding.barcode_sort) {
    std::cout << "Barcode Sort:      Yes (CB: R1 prefix or I1 lane, "
              << cp.encoding.cb_len << " bp) [Legacy]\n";
  }
  std::cout << "Mode:              "
            << (cp.encoding.paired_end ? "Paired-end" : "Single-end") << "\n";
  std::cout << "Reads Processed:   " << cp.read_info.num_reads << "\n";
  if (cp.encoding.paired_end) {
    std::cout << "  (Input 1: " << cp.read_info.num_reads_clean[0]
              << ", Input 2: " << cp.read_info.num_reads_clean[1] << ")\n";
  }

  uint64_t total_orig_compressed_size = cp.gzip.streams[0].compressed_size;
  if (cp.encoding.paired_end) {
    total_orig_compressed_size += cp.gzip.streams[1].compressed_size;
  }
  if (total_orig_compressed_size > 0) {
    double to_mb_factor = 1024.0 * 1024.0;
    double overall_ratio = (double)total_orig_compressed_size / archive_size;
    std::cout << "Compression Ratio: " << std::fixed << std::setprecision(2)
              << overall_ratio << "x ("
              << (uint64_t)(total_orig_compressed_size / to_mb_factor) << " / "
              << (uint64_t)(archive_size / to_mb_factor) << " MB)\n";
  }
  std::cout << "Max Read Length:   " << cp.read_info.max_readlen << " (using "
            << (cp.encoding.long_flag ? "long" : "short") << "-read encoder)\n";
  std::cout << "Preserve Order:    "
            << (cp.encoding.preserve_order ? "Yes" : "No") << "\n";
  std::cout << "Preserve IDs:      " << (cp.encoding.preserve_id ? "Yes" : "No")
            << "\n";
  std::cout << "Preserve Quality:  "
            << (cp.encoding.preserve_quality ? "Yes" : "No") << "\n";
  if (cp.encoding.preserve_quality) {
    std::cout << "Quality Mode:      ";
    if (cp.quality.qvz_flag)
      std::cout << "QVZ (ratio: " << cp.quality.qvz_ratio << ")";
    else if (cp.quality.ill_bin_flag)
      std::cout << "Illumina 8-level binning";
    else if (cp.quality.bin_thr_flag)
      std::cout << "Binary binning (thr: " << cp.quality.bin_thr_thr << ")";
    else
      std::cout << "Lossless";
    std::cout << "\n";
  }
  std::cout << "Compression Level: " << cp.encoding.compression_level << "\n";
  std::cout << "Use CRLF:          " << (cp.encoding.use_crlf ? "Yes" : "No")
            << "\n";

  auto to_mb = [](uint64_t bytes) { return (double)bytes / (1024.0 * 1024.0); };

  auto print_gzip_info = [&](int idx, bool was_gzipped, uint8_t flg,
                             uint32_t mtime, uint8_t xfl, uint8_t os,
                             const std::string &name,
                             const std::string &suggested_name, bool is_bgzf,
                             uint16_t bgzf_bsiz, uint64_t uncomp_sz,
                             uint64_t comp_sz, uint32_t members) {
    if (!was_gzipped)
      return;

    std::cout << "--------------------------------\n";
    std::cout << "Input " << idx << " Original Compression:\n";
    std::string profile = "UNKNOWN";
    if (is_bgzf)
      profile = "BGZF (Default)";
    else if (xfl == 2)
      profile = "MAX (Slowest)";
    else if (xfl == 4)
      profile = "FAST (Fastest)";
    else
      profile = "DEFAULT/OTHER";

    std::cout << "  Profile:         " << profile << "\n";
    std::cout << "  Format:          "
              << (is_bgzf ? "BGZF (Block Gzip)" : "Standard Gzip") << "\n";
    if (is_bgzf) {
      std::cout << "  Block Size:      " << bgzf_bsiz << "\n";
    }
    std::cout << "  Uncompressed Name: "
              << (name.empty() ? suggested_name : name)
              << (name.empty() ? "" : " (from header)") << "\n";
    std::cout << "  Gzip Header:     FLG=0x" << std::hex << (int)flg << std::dec
              << ", MTIME=" << mtime << ", OS=" << (int)os << "\n";
    std::cout << "  Member Count:    " << members << "\n";
    if (comp_sz > 0) {
      double ratio = (double)uncomp_sz / comp_sz;
      std::cout << "  Original Ratio:  " << std::fixed << std::setprecision(2)
                << ratio << "x (" << (uint64_t)to_mb(uncomp_sz) << " / "
                << (uint64_t)to_mb(comp_sz) << " MB)\n";
    }

    std::string origin = "Unknown";
    if (is_bgzf)
      origin = "htslib/samtools/clib";
    else if (mtime == 0 && os == 255)
      origin = "Modern pipeline/programmatic";
    else if (!(flg & 0x08))
      origin = "Programmatic (No filename)";
    std::cout << "  Likely Origin:   " << origin << "\n";
  };

  auto get_suggested_uncomp_name = [](const std::string &path) {
    if (path.size() >= 3 && path.substr(path.size() - 3) == ".gz")
      return path.substr(0, path.size() - 3);
    return path;
  };

  print_gzip_info(
      1, cp.gzip.streams[0].was_gzipped, cp.gzip.streams[0].flg,
      cp.gzip.streams[0].mtime, cp.gzip.streams[0].xfl, cp.gzip.streams[0].os,
      cp.gzip.streams[0].name,
      get_suggested_uncomp_name(cp.read_info.input_filename_1),
      cp.gzip.streams[0].is_bgzf, cp.gzip.streams[0].bgzf_block_size,
      cp.gzip.streams[0].uncompressed_size, cp.gzip.streams[0].compressed_size,
      cp.gzip.streams[0].member_count);

  if (cp.encoding.paired_end) {
    print_gzip_info(
        2, cp.gzip.streams[1].was_gzipped, cp.gzip.streams[1].flg,
        cp.gzip.streams[1].mtime, cp.gzip.streams[1].xfl, cp.gzip.streams[1].os,
        cp.gzip.streams[1].name,
        get_suggested_uncomp_name(cp.read_info.input_filename_2),
        cp.gzip.streams[1].is_bgzf, cp.gzip.streams[1].bgzf_block_size,
        cp.gzip.streams[1].uncompressed_size,
        cp.gzip.streams[1].compressed_size, cp.gzip.streams[1].member_count);
  }
}

void preview(const std::string &archive_path, bool audit_only) {
  auto contents =
      read_files_from_tar_memory(archive_path, {kBundleManifestName});

  if (contents.contains(kBundleManifestName)) {
    const auto manifest = read_key_value_string(contents[kBundleManifestName]);
    if (!manifest.contains("version") ||
        manifest.at("version") != kBundleVersion) {
      throw std::runtime_error("Unsupported grouped archive manifest version.");
    }

    const std::string read_archive_name =
        manifest.contains("read_archive") ? manifest.at("read_archive") : "";
    const std::string index_archive_name =
        manifest.contains("index_archive") ? manifest.at("index_archive") : "";
    const bool has_r3 =
        (manifest.contains("has_r3") && manifest.at("has_r3") == "1");
    const std::string read3_archive_name =
        manifest.contains("read3_archive") ? manifest.at("read3_archive") : "";
    const std::string read3_alias_source =
        manifest.contains("read3_alias_source")
            ? manifest.at("read3_alias_source")
            : "";
    const bool has_index =
        (manifest.contains("has_index") && manifest.at("has_index") == "1");
    const bool has_i2 =
        (manifest.contains("has_i2") && manifest.at("has_i2") == "1");

    std::cout << "SPRING2 Grouped Archive Metadata Preview:\n";
    std::cout << "--------------------------------\n";
    std::cout << "Mode:              Grouped (R + I lanes)\n";
    std::cout << "Input R1:          " << manifest.at("r1_name") << "\n";
    std::cout << "Input R2:          " << manifest.at("r2_name") << "\n";
    if (has_r3 && manifest.contains("r3_name")) {
      std::cout << "Input R3:          " << manifest.at("r3_name") << "\n";
    }
    if (has_index && manifest.contains("i1_name")) {
      std::cout << "Input I1:          " << manifest.at("i1_name") << "\n";
    }
    if (has_index && has_i2 && manifest.contains("i2_name")) {
      std::cout << "Input I2:          " << manifest.at("i2_name") << "\n";
    }
    std::cout << "--------------------------------\n";

    const std::string temp_dir = "tmp_preview_" + random_string(10);
    std::filesystem::create_directories(temp_dir);
    try {
      extract_tar_archive(archive_path, temp_dir);
      std::cout << "\n[Reads Group]\n";
      preview_single(temp_dir + "/" + read_archive_name, false);
      if (has_r3) {
        if (!read3_alias_source.empty()) {
          std::cout << "\n[Read3 Group]\n";
          std::cout << "R3 is aliased to " << read3_alias_source
                    << " (no extra payload stored).\n";
        } else {
          std::cout << "\n[Read3 Group]\n";
          preview_single(temp_dir + "/" + read3_archive_name, false);
        }
      }
      if (has_index) {
        std::cout << "\n[Index Group]\n";
        preview_single(temp_dir + "/" + index_archive_name, false);
      }
    } catch (...) {
      std::error_code ec;
      std::filesystem::remove_all(temp_dir, ec);
      throw;
    }
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
    return;
  }

  preview_single(archive_path, audit_only);
}

} // namespace spring

int main(int argc, char *argv[]) {
  if (argc == 2) {
    const std::string arg = argv[1];
    if (arg == "-V" || arg == "--version") {
      std::cout << "spring2-preview version " << spring::VERSION << "\n";
      return 0;
    }
    if (arg == "-h" || arg == "--help") {
      std::cout << "Usage: spring2-preview [options] <archive.sp>\n";
      std::cout << "Options:\n";
      std::cout << "  -h, --help     Show this help\n";
      std::cout << "  -V, --version  Show version\n";
      std::cout << "  -a, --audit    Perform full archive integrity check\n";
      return 0;
    }
  }

  bool audit_only = false;
  std::string archive_path;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-a" || arg == "--audit") {
      audit_only = true;
    } else if (arg[0] != '-') {
      archive_path = arg;
    }
  }

  if (archive_path.empty()) {
    std::cerr << "Usage: spring2-preview [options] <archive.sp>\n";
    return 1;
  }

  try {
    spring::preview(archive_path, audit_only);
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
