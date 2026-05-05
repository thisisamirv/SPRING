#include "integration_test_support.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace integration_test_support;

namespace {

TEST_CASE("ATAC adapter stripping round-trips and is recorded") {
  const std::string test_dir = "atac_test_tmp";
  fs::create_directories(test_dir);

  const std::string input_fastq = test_dir + "/input.fastq";
  const std::string archive_atac = test_dir + "/test_atac.sp";
  const std::string output_fastq = test_dir + "/roundtrip.fastq";
  const std::string preview_log = test_dir + "/preview.log";

  create_atac_like_fastq(input_fastq, 50000);

  const std::string compress_atac_cmd = std::string(SPRING2_EXECUTABLE) +
                                        " -c --R1 " + input_fastq + " -o " +
                                        archive_atac + " -t 1 -y atac";
  const std::string decompress_atac_cmd = std::string(SPRING2_EXECUTABLE) +
                                          " -d -i " + archive_atac + " -o " +
                                          output_fastq + " -t 1";
  const std::string preview_cmd = std::string(SPRING2_EXECUTABLE) + " -p " +
                                  archive_atac + " > " + preview_log + " 2>&1";

  REQUIRE(std::system(compress_atac_cmd.c_str()) == 0);
  REQUIRE(std::system(decompress_atac_cmd.c_str()) == 0);
  REQUIRE(std::system(preview_cmd.c_str()) == 0);

  {
    std::ifstream preview_in(preview_log, std::ios::binary);
    REQUIRE(preview_in.is_open());
    const std::string preview_output(
        (std::istreambuf_iterator<char>(preview_in)),
        std::istreambuf_iterator<char>());
    CHECK(
        preview_output.find(
            "ATAC Adapters:     Stripped terminal Tn5/Nextera read-through") !=
        std::string::npos);
  }

  CHECK(read_file_binary(output_fastq) == read_file_binary(input_fastq));
  fs::remove_all(test_dir);
}

TEST_CASE("Sparse ATAC read-through keeps adapter stripping disabled") {
  const std::string test_dir = "sparse_atac_test_tmp";
  fs::create_directories(test_dir);

  const std::string input_fastq = test_dir + "/input.fastq";
  const std::string archive_atac = test_dir + "/test_atac.sp";
  const std::string output_fastq = test_dir + "/roundtrip.fastq";
  const std::string preview_log = test_dir + "/preview.log";

  create_sparse_atac_like_fastq(input_fastq, 50000);

  const std::string compress_atac_cmd = std::string(SPRING2_EXECUTABLE) +
                                        " -c --R1 " + input_fastq + " -o " +
                                        archive_atac + " -t 1 -y atac";
  const std::string decompress_atac_cmd = std::string(SPRING2_EXECUTABLE) +
                                          " -d -i " + archive_atac + " -o " +
                                          output_fastq + " -t 1";
  const std::string preview_cmd = std::string(SPRING2_EXECUTABLE) + " -p " +
                                  archive_atac + " > " + preview_log + " 2>&1";

  REQUIRE(std::system(compress_atac_cmd.c_str()) == 0);
  REQUIRE(std::system(decompress_atac_cmd.c_str()) == 0);
  REQUIRE(std::system(preview_cmd.c_str()) == 0);

  {
    std::ifstream preview_in(preview_log, std::ios::binary);
    REQUIRE(preview_in.is_open());
    const std::string preview_output(
        (std::istreambuf_iterator<char>(preview_in)),
        std::istreambuf_iterator<char>());
    CHECK(
        preview_output.find(
            "ATAC Adapters:     Stripped terminal Tn5/Nextera read-through") ==
        std::string::npos);
  }

  CHECK(read_file_binary(output_fastq) == read_file_binary(input_fastq));
  fs::remove_all(test_dir);
}

TEST_CASE("Grouped sc-ATAC auto mode round-trips with N-containing reads") {
  const std::string test_dir = "grouped_sc_atac_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string r3_fastq = test_dir + "/input_R3.fastq";
  const std::string i1_fastq = test_dir + "/input_I1.fastq";
  const std::string archive_path = test_dir + "/grouped_auto.sp";
  const std::string out_r1 = test_dir + "/roundtrip_R1.fastq";
  const std::string out_r2 = test_dir + "/roundtrip_R2.fastq";
  const std::string out_r3 = test_dir + "/roundtrip_R3.fastq";
  const std::string out_i1 = test_dir + "/roundtrip_I1.fastq";
  const std::string preview_log = test_dir + "/preview.log";

  create_atac_like_fastq(r1_fastq, 2000);
  create_atac_like_fastq(r2_fastq, 2000);
  create_dummy_fastq(r3_fastq, 2000);
  create_dummy_fastq(i1_fastq, 2000);

  const std::string compress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + r1_fastq + " --R2 " +
      r2_fastq + " --R3 " + r3_fastq + " --I1 " + i1_fastq + " -o " +
      archive_path + " -t 1 -y auto";
  const std::string decompress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -d -i " + archive_path + " -o " +
      out_r1 + " " + out_r2 + " " + out_r3 + " " + out_i1 + " -t 1";
  const std::string preview_cmd = std::string(SPRING2_EXECUTABLE) + " -p " +
                                  archive_path + " > " + preview_log + " 2>&1";

  REQUIRE(std::system(compress_cmd.c_str()) == 0);
  REQUIRE(std::system(decompress_cmd.c_str()) == 0);
  REQUIRE(std::system(preview_cmd.c_str()) == 0);

  {
    std::ifstream preview_in(preview_log, std::ios::binary);
    REQUIRE(preview_in.is_open());
    const std::string preview_output(
        (std::istreambuf_iterator<char>(preview_in)),
        std::istreambuf_iterator<char>());
    CHECK(preview_output.find("Assay Type:        sc-atac") !=
          std::string::npos);
    CHECK(
        preview_output.find(
            "ATAC Adapters:     Stripped terminal Tn5/Nextera read-through") !=
        std::string::npos);
  }

  for (const auto &[original_path, restored_path] :
       {std::pair{r1_fastq, out_r1}, std::pair{r2_fastq, out_r2},
        std::pair{r3_fastq, out_r3}, std::pair{i1_fastq, out_i1}}) {
    CHECK(read_file_binary(restored_path) == read_file_binary(original_path));
  }

  fs::remove_all(test_dir);
}

TEST_CASE("Grouped sc-RNA index IDs are reconstructed from I1/I2 reads") {
  const std::string test_dir = "grouped_sc_rna_index_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string i1_fastq = test_dir + "/input_I1.fastq";
  const std::string i2_fastq = test_dir + "/input_I2.fastq";
  const std::string archive_auto = test_dir + "/grouped_sc_rna_auto.sp";
  const std::string archive_dna = test_dir + "/grouped_sc_rna_dna.sp";
  const std::string auto_extract_dir = test_dir + "/auto_extract";
  const std::string dna_extract_dir = test_dir + "/dna_extract";
  const std::string auto_index_extract_dir = test_dir + "/auto_index_extract";
  const std::string dna_index_extract_dir = test_dir + "/dna_index_extract";
  const std::string out_r1 = test_dir + "/roundtrip_R1.fastq";
  const std::string out_r2 = test_dir + "/roundtrip_R2.fastq";
  const std::string out_i1 = test_dir + "/roundtrip_I1.fastq";
  const std::string out_i2 = test_dir + "/roundtrip_I2.fastq";
  const std::string preview_log = test_dir + "/preview.log";

  create_grouped_sc_rna_like_fastqs(r1_fastq, r2_fastq, i1_fastq, i2_fastq,
                                    5000);

  const std::string compress_auto_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + r1_fastq + " --R2 " +
      r2_fastq + " --I1 " + i1_fastq + " --I2 " + i2_fastq + " -o " +
      archive_auto + " -t 1 -y sc-rna";
  const std::string compress_dna_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + r1_fastq + " --R2 " +
      r2_fastq + " --I1 " + i1_fastq + " --I2 " + i2_fastq + " -o " +
      archive_dna + " -t 1 -y dna";
  const std::string decompress_auto_cmd =
      std::string(SPRING2_EXECUTABLE) + " -d -i " + archive_auto + " -o " +
      out_r1 + " " + out_r2 + " " + out_i1 + " " + out_i2 + " -t 1";
  const std::string preview_cmd = std::string(SPRING2_EXECUTABLE) + " -p " +
                                  archive_auto + " > " + preview_log + " 2>&1";

  REQUIRE(std::system(compress_auto_cmd.c_str()) == 0);
  REQUIRE(std::system(compress_dna_cmd.c_str()) == 0);
  REQUIRE(std::system(decompress_auto_cmd.c_str()) == 0);
  REQUIRE(std::system(preview_cmd.c_str()) == 0);

  fs::create_directories(auto_extract_dir);
  fs::create_directories(dna_extract_dir);
  REQUIRE(std::system(("tar -xf " + archive_auto + " -C " + auto_extract_dir)
                          .c_str()) == 0);
  REQUIRE(std::system(
              ("tar -xf " + archive_dna + " -C " + dna_extract_dir).c_str()) ==
          0);
  fs::create_directories(auto_index_extract_dir);
  fs::create_directories(dna_index_extract_dir);
  REQUIRE(std::system(("tar -xf " + auto_extract_dir + "/index_group.sp -C " +
                       auto_index_extract_dir)
                          .c_str()) == 0);
  REQUIRE(std::system(("tar -xf " + dna_extract_dir + "/index_group.sp -C " +
                       dna_index_extract_dir)
                          .c_str()) == 0);

  CHECK(fs::file_size(auto_index_extract_dir + "/id_1.0") <
        fs::file_size(dna_index_extract_dir + "/id_1.0"));

  {
    std::ifstream preview_in(preview_log, std::ios::binary);
    REQUIRE(preview_in.is_open());
    const std::string preview_output(
        (std::istreambuf_iterator<char>(preview_in)),
        std::istreambuf_iterator<char>());
    CHECK(preview_output.find("Index IDs:         Reconstructed trailing I1/I2 "
                              "token from index reads") != std::string::npos);
  }

  for (const auto &[original_path, restored_path] :
       {std::pair{r1_fastq, out_r1}, std::pair{r2_fastq, out_r2},
        std::pair{i1_fastq, out_i1}, std::pair{i2_fastq, out_i2}}) {
    CHECK(read_file_binary(restored_path) == read_file_binary(original_path));
  }

  fs::remove_all(test_dir);
}

} // namespace