#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "spring_reader.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

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
  std::string preview_path =
      fs::path(spring2_path).parent_path().string() + "/spring2-preview";

  std::string audit_cmd = preview_path + " -a " + archive_sp;
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
      preview_path + " -a " + corrupted_sp + " > " + audit_log + " 2>&1";
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

} // namespace
