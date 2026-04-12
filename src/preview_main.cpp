// Implements the standalone spring2-preview utility for extracting and displaying
// archive metadata, original filenames, and format notes without decompression.

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

#include "spring.h"
#include "util.h"

namespace spring {

void preview(const std::string &archive_path) {
  const std::string temp_dir = "tmp_preview_" + random_string(10);
  std::filesystem::create_directories(temp_dir);

  try {
    const std::string untar_command =
        "tar -xf " + shell_quote(shell_path(archive_path)) + " -C " +
        shell_quote(shell_path(temp_dir)) +
        " --wildcards '*cp.bin' 2>/dev/null || " + "tar -xf " +
        shell_quote(shell_path(archive_path)) + " -C " +
        shell_quote(shell_path(temp_dir)) + " cp.bin ./cp.bin";

    if (std::system(untar_command.c_str()) != 0) {
      throw std::runtime_error("Failed to extract metadata from archive.");
    }

    compression_params cp{};
    std::ifstream in(temp_dir + "/cp.bin", std::ios::binary);
    if (!in.is_open()) {
      throw std::runtime_error("Can't open parameter file after extraction.");
    }
    read_compression_params(in, cp);
    in.close();

    std::cout << "SPRING2 Archive Metadata Preview:\n";
    std::cout << "--------------------------------\n";
    std::cout << "Original Input 1:  " << cp.input_filename_1 << "\n";
    if (cp.paired_end) {
      std::cout << "Original Input 2:  " << cp.input_filename_2 << "\n";
    }
    uint64_t archive_size = std::filesystem::file_size(archive_path);
    if (!cp.note.empty()) {
      std::cout << "Note:              " << cp.note << "\n";
    }
    std::cout << "Mode:              "
              << (cp.paired_end ? "Paired-end" : "Single-end") << "\n";
    std::cout << "Reads Processed:   " << cp.num_reads << "\n";
    if (cp.paired_end) {
      std::cout << "  (Input 1: " << cp.num_reads_clean[0]
                << ", Input 2: " << cp.num_reads_clean[1] << ")\n";
    }

    uint64_t total_orig_compressed_size = cp.input_1_gzip_compressed_size;
    if (cp.paired_end) {
      total_orig_compressed_size += cp.input_2_gzip_compressed_size;
    }
    if (total_orig_compressed_size > 0) {
      double to_mb_factor = 1024.0 * 1024.0;
      double overall_ratio = (double)total_orig_compressed_size / archive_size;
      std::cout << "Compression Ratio: " << std::fixed << std::setprecision(2)
                << overall_ratio << "x ("
                << (uint64_t)(total_orig_compressed_size / to_mb_factor) << " / "
                << (uint64_t)(archive_size / to_mb_factor) << " MB)\n";
    }
    std::cout << "Max Read Length:   " << cp.max_readlen << " (using "
              << (cp.long_flag ? "long" : "short") << "-read encoder)\n";
    std::cout << "Preserve Order:    " << (cp.preserve_order ? "Yes" : "No")
              << "\n";
    std::cout << "Preserve IDs:      " << (cp.preserve_id ? "Yes" : "No")
              << "\n";
    std::cout << "Preserve Quality:  " << (cp.preserve_quality ? "Yes" : "No")
              << "\n";
    if (cp.preserve_quality) {
      std::cout << "Quality Mode:      ";
      if (cp.qvz_flag)
        std::cout << "QVZ (ratio: " << cp.qvz_ratio << ")";
      else if (cp.ill_bin_flag)
        std::cout << "Illumina 8-level binning";
      else if (cp.bin_thr_flag)
        std::cout << "Binary binning (thr: " << cp.bin_thr_thr << ")";
      else
        std::cout << "Lossless";
      std::cout << "\n";
    }
    std::cout << "Compression Level: " << cp.compression_level << "\n";
    std::cout << "Use CRLF:          " << (cp.use_crlf ? "Yes" : "No") << "\n";

    auto to_mb = [](uint64_t bytes) {
      return (double)bytes / (1024.0 * 1024.0);
    };

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
      std::cout << "  Uncompressed Name: " << (name.empty() ? suggested_name : name)
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

    print_gzip_info(1, cp.input_1_was_gzipped, cp.input_1_gzip_flg,
                    cp.input_1_gzip_mtime, cp.input_1_gzip_xfl,
                    cp.input_1_gzip_os, cp.input_1_gzip_name,
                    get_suggested_uncomp_name(cp.input_filename_1),
                    cp.input_1_is_bgzf, cp.input_1_bgzf_block_size,
                    cp.input_1_gzip_uncompressed_size,
                    cp.input_1_gzip_compressed_size,
                    cp.input_1_gzip_member_count);

    if (cp.paired_end) {
      print_gzip_info(2, cp.input_2_was_gzipped, cp.input_2_gzip_flg,
                      cp.input_2_gzip_mtime, cp.input_2_gzip_xfl,
                      cp.input_2_gzip_os, cp.input_2_gzip_name,
                      get_suggested_uncomp_name(cp.input_filename_2),
                      cp.input_2_is_bgzf, cp.input_2_bgzf_block_size,
                      cp.input_2_gzip_uncompressed_size,
                      cp.input_2_gzip_compressed_size,
                      cp.input_2_gzip_member_count);
    }

    std::cout << "--------------------------------\n";

  } catch (const std::exception &e) {
    std::filesystem::remove_all(temp_dir);
    throw;
  }
  std::filesystem::remove_all(temp_dir);
}

} // namespace spring

int main(int argc, char *argv[]) {
  if (argc != 2) {
    std::cerr << "Usage: spring2-preview <archive.sp>\n";
    return 1;
  }

  try {
    spring::preview(argv[1]);
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }

  return 0;
}
