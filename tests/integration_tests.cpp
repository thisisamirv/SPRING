#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "spring_reader.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#ifndef SPRING2_EXECUTABLE
#define SPRING2_EXECUTABLE "spring2"
#endif

namespace fs = std::filesystem;
using namespace spring;

namespace {

void create_dummy_fastq(const std::string &path, int num_records) {
  std::ofstream ofs(path, std::ios::binary);
  constexpr int read_len = 80;
  for (int i = 0; i < num_records; ++i) {
    std::string read;
    read.reserve(read_len);
    static constexpr char kBaseCycle[] = {'A', 'C', 'G', 'T'};
    for (int j = 0; j < read_len; ++j) {
      read.push_back(kBaseCycle[(i + j) % 4]);
    }

    ofs << "@read_" << i << "\n";
    ofs << read << "\n";
    ofs << "+\n";
    ofs << std::string(read_len, 'I') << "\n";
  }
}

void create_atac_like_fastq(const std::string &path, int num_records) {
  std::ofstream ofs(path, std::ios::binary);
  static constexpr std::string_view kAdapter = "CTGTCTCTTATACACATCT";
  constexpr int read_len = 80;
  constexpr int overlap = static_cast<int>(kAdapter.size());

  for (int i = 0; i < num_records; ++i) {
    const int insert_len = read_len - overlap;
    std::string read;
    read.reserve(read_len);
    static constexpr char kBaseCycle[] = {'A', 'C', 'G', 'T'};
    uint32_t state = static_cast<uint32_t>(0x9E3779B9u ^ (i * 2654435761u));
    for (int j = 0; j < insert_len; ++j) {
      state = state * 1664525u + 1013904223u;
      read.push_back(kBaseCycle[(state >> 30) & 0x03]);
    }
    if (i % 7 == 0) {
      read[static_cast<size_t>((i / 7) % insert_len)] = 'N';
    }
    read.append(kAdapter);

    ofs << "@atac_read_" << i << "\n";
    ofs << read << "\n";
    ofs << "+\n";
    ofs << std::string(static_cast<size_t>(read_len), 'I') << "\n";
  }
}

void create_sparse_atac_like_fastq(const std::string &path, int num_records) {
  std::ofstream ofs(path, std::ios::binary);
  static constexpr std::string_view kAdapter = "CTGTCTCTTATACACATCT";
  constexpr int read_len = 80;
  constexpr int overlap = 14;

  for (int i = 0; i < num_records; ++i) {
    std::string read;
    read.reserve(read_len);
    static constexpr char kBaseCycle[] = {'A', 'C', 'G', 'T'};
    uint32_t state = static_cast<uint32_t>(0x7F4A7C15u ^ (i * 2246822519u));
    for (int j = 0; j < read_len; ++j) {
      state = state * 1664525u + 1013904223u;
      read.push_back(kBaseCycle[(state >> 30) & 0x03]);
    }
    if (i % 64 == 0) {
      read.replace(static_cast<size_t>(read_len - overlap), overlap,
                   std::string(kAdapter.substr(0, overlap)));
    }

    ofs << "@sparse_atac_read_" << i << "\n";
    ofs << read << "\n";
    ofs << "+\n";
    ofs << std::string(static_cast<size_t>(read_len), 'I') << "\n";
  }
}

void create_grouped_sc_rna_like_fastqs(const std::string &r1_path,
                                       const std::string &r2_path,
                                       const std::string &i1_path,
                                       const std::string &i2_path,
                                       int num_records) {
  std::ofstream r1(r1_path, std::ios::binary);
  std::ofstream r2(r2_path, std::ios::binary);
  std::ofstream i1(i1_path, std::ios::binary);
  std::ofstream i2(i2_path, std::ios::binary);
  constexpr int read_len = 151;
  static constexpr char kBaseCycle[] = {'A', 'C', 'G', 'T'};

  for (int record = 0; record < num_records; ++record) {
    std::string cb1(10, 'A');
    std::string cb2(10, 'C');
    for (int j = 0; j < 10; ++j) {
      cb1[static_cast<size_t>(j)] = kBaseCycle[(record + j) % 4];
      cb2[static_cast<size_t>(j)] = kBaseCycle[((record / 4) + j + 1) % 4];
    }

    std::string seq1;
    std::string seq2;
    seq1.reserve(read_len);
    seq2.reserve(read_len);
    for (int j = 0; j < read_len; ++j) {
      seq1.push_back(kBaseCycle[(record + j * 3) % 4]);
      seq2.push_back(kBaseCycle[(record + j * 5 + 1) % 4]);
    }
    seq1.replace(static_cast<size_t>(read_len - 30), 30, std::string(30, 'T'));
    seq2.replace(static_cast<size_t>(read_len - 28), 28, std::string(28, 'A'));

    const std::string suffix = cb1 + "+" + cb2;
    const std::string id1 =
        "@scrna_" + std::to_string(record) + " 1:N:0:" + suffix;
    const std::string id2 =
        "@scrna_" + std::to_string(record) + " 2:N:0:" + suffix;

    r1 << id1 << "\n"
       << seq1 << "\n+\n"
       << std::string(static_cast<size_t>(read_len), 'I') << "\n";
    r2 << id2 << "\n"
       << seq2 << "\n+\n"
       << std::string(static_cast<size_t>(read_len), 'I') << "\n";
    i1 << id1 << "\n" << cb1 << "\n+\n" << std::string(10, 'I') << "\n";
    i2 << id2 << "\n" << cb2 << "\n+\n" << std::string(10, 'I') << "\n";
  }
}

std::string read_manifest_value(const std::string &manifest_path,
                                const std::string &key) {
  std::ifstream input(manifest_path, std::ios::binary);
  if (!input.is_open())
    return "";

  std::string line;
  const std::string prefix = key + "=";
  while (std::getline(input, line)) {
    if (line.rfind(prefix, 0) == 0)
      return line.substr(prefix.size());
  }
  return "";
}

TEST_CASE("Archive Integrity Verification Test") {
  std::string test_dir = "integrity_test_tmp";
  fs::create_directories(test_dir);

  std::string input_fastq = test_dir + "/input.fastq";
  std::string archive_sp = test_dir + "/test.sp";

  int num_records = 100;
  create_dummy_fastq(input_fastq, num_records);

  // 1. Compress with mandatory audit
  std::string compress_cmd = std::string(SPRING2_EXECUTABLE) + " -c -a --R1 " +
                             input_fastq + " -o " + archive_sp + " -w " +
                             test_dir + "/work_compress -t 1";
  REQUIRE(std::system(compress_cmd.c_str()) == 0);

  // 2. Verify (should pass)
  std::string spring2_path = SPRING2_EXECUTABLE;
  std::string audit_cmd = spring2_path + " -p -a " + archive_sp;
  CHECK(std::system(audit_cmd.c_str()) == 0);

  // 3. Corrupt the archive
  std::string corrupt_dir = test_dir + "/corrupt_work";
  fs::create_directories(corrupt_dir);

  std::string untar_cmd = "tar -xf " + archive_sp + " -C " + corrupt_dir;
  REQUIRE(std::system(untar_cmd.c_str()) == 0);

  // Truncate cp.bin so audit must fail while parsing metadata/checkpoints.
  const fs::path cp_path = fs::path(corrupt_dir) / "cp.bin";
  REQUIRE(fs::exists(cp_path));
  const auto cp_size = fs::file_size(cp_path);
  REQUIRE(cp_size > 16);
  fs::resize_file(cp_path, cp_size / 2);

  // Retar
  std::string corrupted_sp = test_dir + "/corrupted.sp";
  std::string retar_cmd =
      "cd " + corrupt_dir + " && tar -cf ../../" + corrupted_sp + " *";
  REQUIRE(std::system(retar_cmd.c_str()) == 0);

  // 4. Audit corrupted archive: accept either hard failure (nonzero)
  // or clearly corrupted metadata output.
  std::string audit_log = test_dir + "/corrupt_audit.log";
  std::string audit_corrupt_cmd =
      spring2_path + " -p -a " + corrupted_sp + " > " + audit_log + " 2>&1";
  int ret = std::system(audit_corrupt_cmd.c_str());

  bool audit_detected_corruption = (ret != 0);
  if (!audit_detected_corruption) {
    std::ifstream ifs(audit_log, std::ios::binary);
    std::string output((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
    audit_detected_corruption =
        output.find("Original Input 1:  input.fastq") == std::string::npos;
  }
  CHECK(audit_detected_corruption);

  fs::remove_all(test_dir);
}

TEST_CASE("SpringReader Integration Test") {
  std::string test_dir = "reader_test_tmp";
  fs::create_directories(test_dir);

  std::string input_fastq = test_dir + "/input.fastq";
  std::string archive_spring = test_dir + "/test.spring";

  int num_records = 100;
  create_dummy_fastq(input_fastq, num_records);

  // Compress using the spring2 binary we just built
  std::string compress_cmd = std::string(SPRING2_EXECUTABLE) + " -c --R1 " +
                             input_fastq + " -o " + archive_spring + " -w " +
                             test_dir + "/work_compress -t 1";
  int ret = std::system(compress_cmd.c_str());
  REQUIRE(ret == 0);

  SUBCASE("Stream decompression (Single End)") {
    SpringReader reader(archive_spring, 1, test_dir + "/work_reader");

    ReadRecord rec;
    int count = 0;
    while (reader.next(rec)) {
      CHECK(rec.id == "@read_" + std::to_string(count));
      count++;
    }
    CHECK(count == num_records);
  }

  fs::remove_all(test_dir);
}

TEST_CASE("Multi-thread compression compatibility") {
  std::string test_dir = "thread_test_tmp";
  fs::create_directories(test_dir);

  std::string input_fastq = test_dir + "/input.fastq";

  // Create a slightly larger dataset to test threading behavior
  int num_records = 1000;
  create_dummy_fastq(input_fastq, num_records);

  // Test compression with different thread counts and verify each archive
  // can be decompressed with a different thread count (regression test for
  // decompression thread count mismatch bug)
  for (int compress_threads : {1, 4, 8}) {
    std::string archive_sp =
        test_dir + "/test_t" + std::to_string(compress_threads) + ".sp";
    std::string work_compress =
        test_dir + "/work_compress_t" + std::to_string(compress_threads);

    // Compress with N threads
    std::string compress_cmd = std::string(SPRING2_EXECUTABLE) + " -c --R1 " +
                               input_fastq + " -o " + archive_sp + " -w " +
                               work_compress + " -t " +
                               std::to_string(compress_threads);
    REQUIRE(std::system(compress_cmd.c_str()) == 0);

    // Decompress with 1 thread (different from compression thread count)
    // This validates that archive thread count is properly used internally
    std::string output_fastq =
        test_dir + "/output_t" + std::to_string(compress_threads) + ".fastq";
    std::string work_decompress =
        test_dir + "/work_decompress_t" + std::to_string(compress_threads);
    std::string decompress_cmd = std::string(SPRING2_EXECUTABLE) + " -d -i " +
                                 archive_sp + " -o " + output_fastq + " -w " +
                                 work_decompress + " -t 1";

    // Exit code 0 means integrity check passed
    CHECK(std::system(decompress_cmd.c_str()) == 0);
  }

  fs::remove_all(test_dir);
}

TEST_CASE("ATAC adapter stripping round-trips and is recorded") {
  const std::string test_dir = "atac_test_tmp";
  fs::create_directories(test_dir);

  const std::string input_fastq = test_dir + "/input.fastq";
  const std::string archive_atac = test_dir + "/test_atac.sp";
  const std::string output_fastq = test_dir + "/roundtrip.fastq";
  const std::string preview_log = test_dir + "/preview.log";

  create_atac_like_fastq(input_fastq, 50000);

  const std::string compress_atac_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + input_fastq + " -o " +
      archive_atac + " -w " + test_dir + "/work_atac -t 1 -y atac";
  const std::string decompress_atac_cmd =
      std::string(SPRING2_EXECUTABLE) + " -d -i " + archive_atac + " -o " +
      output_fastq + " -w " + test_dir + "/work_roundtrip -t 1";
  const std::string spring2_path = SPRING2_EXECUTABLE;
  const std::string preview_cmd =
      spring2_path + " -p " + archive_atac + " > " + preview_log + " 2>&1";

  REQUIRE(std::system(compress_atac_cmd.c_str()) == 0);
  REQUIRE(std::system(decompress_atac_cmd.c_str()) == 0);
  REQUIRE(std::system(preview_cmd.c_str()) == 0);

  std::ifstream preview_in(preview_log, std::ios::binary);
  REQUIRE(preview_in.is_open());
  const std::string preview_output((std::istreambuf_iterator<char>(preview_in)),
                                   std::istreambuf_iterator<char>());
  CHECK(preview_output.find(
            "ATAC Adapters:     Stripped terminal Tn5/Nextera read-through") !=
        std::string::npos);
  preview_in.close();

  std::ifstream original_in(input_fastq, std::ios::binary);
  std::ifstream restored_in(output_fastq, std::ios::binary);
  REQUIRE(original_in.is_open());
  REQUIRE(restored_in.is_open());
  const std::string original((std::istreambuf_iterator<char>(original_in)),
                             std::istreambuf_iterator<char>());
  const std::string restored((std::istreambuf_iterator<char>(restored_in)),
                             std::istreambuf_iterator<char>());
  CHECK(restored == original);
  original_in.close();
  restored_in.close();

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

  const std::string compress_atac_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + input_fastq + " -o " +
      archive_atac + " -w " + test_dir + "/work_atac -t 1 -y atac";
  const std::string decompress_atac_cmd =
      std::string(SPRING2_EXECUTABLE) + " -d -i " + archive_atac + " -o " +
      output_fastq + " -w " + test_dir + "/work_roundtrip -t 1";
  const std::string spring2_path = SPRING2_EXECUTABLE;
  const std::string preview_cmd =
      spring2_path + " -p " + archive_atac + " > " + preview_log + " 2>&1";

  REQUIRE(std::system(compress_atac_cmd.c_str()) == 0);
  REQUIRE(std::system(decompress_atac_cmd.c_str()) == 0);
  REQUIRE(std::system(preview_cmd.c_str()) == 0);

  std::ifstream preview_in(preview_log, std::ios::binary);
  REQUIRE(preview_in.is_open());
  const std::string preview_output((std::istreambuf_iterator<char>(preview_in)),
                                   std::istreambuf_iterator<char>());
  CHECK(preview_output.find(
            "ATAC Adapters:     Stripped terminal Tn5/Nextera read-through") ==
        std::string::npos);
  preview_in.close();

  std::ifstream original_in(input_fastq, std::ios::binary);
  std::ifstream restored_in(output_fastq, std::ios::binary);
  REQUIRE(original_in.is_open());
  REQUIRE(restored_in.is_open());
  const std::string original((std::istreambuf_iterator<char>(original_in)),
                             std::istreambuf_iterator<char>());
  const std::string restored((std::istreambuf_iterator<char>(restored_in)),
                             std::istreambuf_iterator<char>());
  CHECK(restored == original);
  original_in.close();
  restored_in.close();

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
      archive_path + " -w " + test_dir + "/work_compress -t 1 -y auto";
  const std::string decompress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -d -i " + archive_path + " -o " +
      out_r1 + " " + out_r2 + " " + out_r3 + " " + out_i1 + " -w " + test_dir +
      "/work_decompress -t 1";
  const std::string spring2_path = SPRING2_EXECUTABLE;
  const std::string preview_cmd =
      spring2_path + " -p " + archive_path + " > " + preview_log + " 2>&1";

  REQUIRE(std::system(compress_cmd.c_str()) == 0);
  REQUIRE(std::system(decompress_cmd.c_str()) == 0);
  REQUIRE(std::system(preview_cmd.c_str()) == 0);

  std::ifstream preview_in(preview_log, std::ios::binary);
  REQUIRE(preview_in.is_open());
  const std::string preview_output((std::istreambuf_iterator<char>(preview_in)),
                                   std::istreambuf_iterator<char>());
  CHECK(preview_output.find("Assay Type:        sc-atac") != std::string::npos);
  CHECK(preview_output.find(
            "ATAC Adapters:     Stripped terminal Tn5/Nextera read-through") !=
        std::string::npos);
  preview_in.close();

  for (const auto &[original_path, restored_path] :
       {std::pair{r1_fastq, out_r1}, std::pair{r2_fastq, out_r2},
        std::pair{r3_fastq, out_r3}, std::pair{i1_fastq, out_i1}}) {
    std::ifstream original_in(original_path, std::ios::binary);
    std::ifstream restored_in(restored_path, std::ios::binary);
    REQUIRE(original_in.is_open());
    REQUIRE(restored_in.is_open());
    const std::string original((std::istreambuf_iterator<char>(original_in)),
                               std::istreambuf_iterator<char>());
    const std::string restored((std::istreambuf_iterator<char>(restored_in)),
                               std::istreambuf_iterator<char>());
    CHECK(restored == original);
  }

  fs::remove_all(test_dir);
}

TEST_CASE(
    "Grouped preview audit detects corruption and preserves archive note") {
  const std::string test_dir = "grouped_preview_audit_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string r3_fastq = test_dir + "/input_R3.fastq";
  const std::string i1_fastq = test_dir + "/input_I1.fastq";
  const std::string archive_path = test_dir + "/grouped_note.sp";
  const std::string preview_log = test_dir + "/preview.log";
  const std::string corrupt_dir = test_dir + "/corrupt_extract";
  const std::string nested_read_dir = test_dir + "/nested_read_extract";
  const std::string corrupt_log = test_dir + "/corrupt_audit.log";
  const fs::path corrupted_archive_path =
      fs::absolute(fs::path(test_dir) / "grouped_corrupted.sp");

  create_dummy_fastq(r1_fastq, 500);
  create_dummy_fastq(r2_fastq, 500);
  create_dummy_fastq(r3_fastq, 500);
  create_dummy_fastq(i1_fastq, 500);

  const std::string compress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + r1_fastq + " --R2 " +
      r2_fastq + " --R3 " + r3_fastq + " --I1 " + i1_fastq +
      " -n GROUPED_TEST_NOTE -o " + archive_path + " -w " + test_dir +
      "/work_compress -t 1 -y auto";
  const std::string preview_cmd = std::string(SPRING2_EXECUTABLE) + " -p " +
                                  archive_path + " > " + preview_log + " 2>&1";
  const std::string audit_cmd =
      std::string(SPRING2_EXECUTABLE) + " -p -a " + archive_path;

  REQUIRE(std::system(compress_cmd.c_str()) == 0);
  REQUIRE(std::system(preview_cmd.c_str()) == 0);
  REQUIRE(std::system(audit_cmd.c_str()) == 0);

  std::ifstream preview_in(preview_log, std::ios::binary);
  REQUIRE(preview_in.is_open());
  const std::string preview_output((std::istreambuf_iterator<char>(preview_in)),
                                   std::istreambuf_iterator<char>());
  CHECK(preview_output.find("Note:              GROUPED_TEST_NOTE") !=
        std::string::npos);
  preview_in.close();

  fs::create_directories(corrupt_dir);
  REQUIRE(std::system(
              ("tar -xf " + archive_path + " -C " + corrupt_dir).c_str()) == 0);

  fs::create_directories(nested_read_dir);
  const std::string read_archive_name =
      read_manifest_value(corrupt_dir + "/bundle.meta", "read_archive");
  REQUIRE(!read_archive_name.empty());
  const fs::path read_archive = fs::path(corrupt_dir) / read_archive_name;
  const std::string read_archive_tar_path =
      fs::absolute(read_archive).generic_string();
  const std::string corrupted_archive_tar_path =
      corrupted_archive_path.generic_string();
  REQUIRE(fs::exists(read_archive));
  REQUIRE(std::system(
              ("tar -xf " + read_archive.string() + " -C " + nested_read_dir)
                  .c_str()) == 0);

  const fs::path cp_path = fs::path(nested_read_dir) / "cp.bin";
  REQUIRE(fs::exists(cp_path));
  const auto cp_size = fs::file_size(cp_path);
  REQUIRE(cp_size > 16);
  fs::resize_file(cp_path, cp_size / 2);

  REQUIRE(std::system(("cd " + nested_read_dir + " && tar -cf \"" +
                       read_archive_tar_path + "\" *")
                          .c_str()) == 0);

  REQUIRE(std::system(("cd " + corrupt_dir + " && tar -cf \"" +
                       corrupted_archive_tar_path + "\" *")
                          .c_str()) == 0);

  const std::string corrupt_audit_cmd = std::string(SPRING2_EXECUTABLE) +
                                        " -p -a " + corrupted_archive_tar_path +
                                        " > " + corrupt_log + " 2>&1";
  CHECK(std::system(corrupt_audit_cmd.c_str()) != 0);

  fs::remove_all(test_dir);
}

TEST_CASE("Grouped decompression accepts five explicit output files") {
  const std::string test_dir = "grouped_five_output_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string r3_fastq = test_dir + "/input_R3.fastq";
  const std::string i1_fastq = test_dir + "/input_I1.fastq";
  const std::string i2_fastq = test_dir + "/input_I2.fastq";
  const std::string archive_path = test_dir + "/grouped_five_out.sp";
  const std::string out_r1 = test_dir + "/out_R1.fastq";
  const std::string out_r2 = test_dir + "/out_R2.fastq";
  const std::string out_r3 = test_dir + "/out_R3.fastq";
  const std::string out_i1 = test_dir + "/out_I1.fastq";
  const std::string out_i2 = test_dir + "/out_I2.fastq";

  create_grouped_sc_rna_like_fastqs(r1_fastq, r2_fastq, i1_fastq, i2_fastq,
                                    1000);
  create_dummy_fastq(r3_fastq, 1000);

  const std::string compress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + r1_fastq + " --R2 " +
      r2_fastq + " --R3 " + r3_fastq + " --I1 " + i1_fastq + " --I2 " +
      i2_fastq + " -o " + archive_path + " -w " + test_dir +
      "/work_compress -t 1 -y sc-rna";
  const std::string decompress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -d -i " + archive_path + " -o " +
      out_r1 + " " + out_r2 + " " + out_r3 + " " + out_i1 + " " + out_i2 +
      " -w " + test_dir + "/work_decompress -t 1";

  REQUIRE(std::system(compress_cmd.c_str()) == 0);
  REQUIRE(std::system(decompress_cmd.c_str()) == 0);

  for (const auto &[original_path, restored_path] :
       {std::pair{r1_fastq, out_r1}, std::pair{r2_fastq, out_r2},
        std::pair{r3_fastq, out_r3}, std::pair{i1_fastq, out_i1},
        std::pair{i2_fastq, out_i2}}) {
    std::ifstream original_in(original_path, std::ios::binary);
    std::ifstream restored_in(restored_path, std::ios::binary);
    REQUIRE(original_in.is_open());
    REQUIRE(restored_in.is_open());
    const std::string original((std::istreambuf_iterator<char>(original_in)),
                               std::istreambuf_iterator<char>());
    const std::string restored((std::istreambuf_iterator<char>(restored_in)),
                               std::istreambuf_iterator<char>());
    CHECK(restored == original);
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
      archive_auto + " -w " + test_dir + "/work_auto -t 1 -y sc-rna";
  const std::string compress_dna_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + r1_fastq + " --R2 " +
      r2_fastq + " --I1 " + i1_fastq + " --I2 " + i2_fastq + " -o " +
      archive_dna + " -w " + test_dir + "/work_dna -t 1 -y dna";
  const std::string decompress_auto_cmd =
      std::string(SPRING2_EXECUTABLE) + " -d -i " + archive_auto + " -o " +
      out_r1 + " " + out_r2 + " " + out_i1 + " " + out_i2 + " -w " + test_dir +
      "/work_roundtrip -t 1";
  const std::string spring2_path = SPRING2_EXECUTABLE;
  const std::string preview_cmd =
      spring2_path + " -p " + archive_auto + " > " + preview_log + " 2>&1";

  REQUIRE(std::system(compress_auto_cmd.c_str()) == 0);
  REQUIRE(std::system(compress_dna_cmd.c_str()) == 0);
  REQUIRE(std::system(decompress_auto_cmd.c_str()) == 0);
  REQUIRE(std::system(preview_cmd.c_str()) == 0);

  fs::create_directories(auto_extract_dir);
  fs::create_directories(dna_extract_dir);
  const std::string untar_auto_cmd =
      "tar -xf " + archive_auto + " -C " + auto_extract_dir;
  const std::string untar_dna_cmd =
      "tar -xf " + archive_dna + " -C " + dna_extract_dir;
  REQUIRE(std::system(untar_auto_cmd.c_str()) == 0);
  REQUIRE(std::system(untar_dna_cmd.c_str()) == 0);
  fs::create_directories(auto_index_extract_dir);
  fs::create_directories(dna_index_extract_dir);
  const std::string untar_auto_index_cmd = "tar -xf " + auto_extract_dir +
                                           "/index_group.sp -C " +
                                           auto_index_extract_dir;
  const std::string untar_dna_index_cmd = "tar -xf " + dna_extract_dir +
                                          "/index_group.sp -C " +
                                          dna_index_extract_dir;
  REQUIRE(std::system(untar_auto_index_cmd.c_str()) == 0);
  REQUIRE(std::system(untar_dna_index_cmd.c_str()) == 0);

  CHECK(fs::file_size(auto_index_extract_dir + "/id_1.0") <
        fs::file_size(dna_index_extract_dir + "/id_1.0"));

  std::ifstream preview_in(preview_log, std::ios::binary);
  REQUIRE(preview_in.is_open());
  const std::string preview_output((std::istreambuf_iterator<char>(preview_in)),
                                   std::istreambuf_iterator<char>());
  CHECK(preview_output.find("Index IDs:         Reconstructed trailing I1/I2 "
                            "token from index reads") != std::string::npos);
  preview_in.close();

  for (const auto &[original_path, restored_path] :
       {std::pair{r1_fastq, out_r1}, std::pair{r2_fastq, out_r2},
        std::pair{i1_fastq, out_i1}, std::pair{i2_fastq, out_i2}}) {
    std::ifstream original_in(original_path, std::ios::binary);
    std::ifstream restored_in(restored_path, std::ios::binary);
    REQUIRE(original_in.is_open());
    REQUIRE(restored_in.is_open());
    const std::string original((std::istreambuf_iterator<char>(original_in)),
                               std::istreambuf_iterator<char>());
    const std::string restored((std::istreambuf_iterator<char>(restored_in)),
                               std::istreambuf_iterator<char>());
    CHECK(restored == original);
  }

  fs::remove_all(test_dir);
}

} // namespace
