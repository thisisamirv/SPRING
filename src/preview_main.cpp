#include <filesystem>
#include <fstream>
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
