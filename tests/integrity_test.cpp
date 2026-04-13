#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

void create_dummy_fastq(const std::string &path, int num_records) {
  std::ofstream ofs(path);
  for (int i = 0; i < num_records; ++i) {
    ofs << "@read_" << i << "\n";
    ofs << "ACTGN"[i % 5] << "CTGAN"[i % 5] << "GTCAN"[i % 5] << "TGACN"[i % 5]
        << "\n";
    ofs << "+\n";
    ofs << "!!!!" << "\n";
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
  std::string compress_cmd = std::string(SPRING2_EXECUTABLE) + " -c -a -i " +
                             input_fastq + " -o " + archive_sp + " -w " +
                             test_dir + "/work_compress -t 1";
  REQUIRE(std::system(compress_cmd.c_str()) == 0);

  // 2. Verify (should pass)
  // We need to find the spring2-preview binary. It should be in the same dir as
  // spring2
  std::string spring2_path = SPRING2_EXECUTABLE;
  std::string preview_path =
      fs::path(spring2_path).parent_path().string() + "/spring2-preview";

  std::string audit_cmd = preview_path + " -a " + archive_sp;
  CHECK(std::system(audit_cmd.c_str()) == 0);

  // 3. Corrupt the archive
  // We'll untar it, corrupt the dna stream, and retar it.
  std::string corrupt_dir = test_dir + "/corrupt_work";
  fs::create_directories(corrupt_dir);

  std::string untar_cmd = "tar -xf " + archive_sp + " -C " + corrupt_dir;
  REQUIRE(std::system(untar_cmd.c_str()) == 0);

  // Find any stream file to corrupt.
  std::string stream_file = "";
  for (const auto &entry : fs::recursive_directory_iterator(corrupt_dir)) {
    if (entry.is_regular_file() && entry.path().filename() != "cp.bin") {
      stream_file = entry.path().string();
      break;
    }
  }

  REQUIRE(!stream_file.empty());
  REQUIRE(fs::exists(stream_file));

  // Corrupt one byte
  {
    std::fstream fs_stream(stream_file,
                           std::ios::binary | std::ios::in | std::ios::out);
    fs_stream.seekg(10);
    char b;
    fs_stream.read(&b, 1);
    b ^= 0xFF;
    fs_stream.seekp(10);
    fs_stream.write(&b, 1);
  }

  // Retar
  std::string corrupted_sp = test_dir + "/corrupted.sp";
  std::string retar_cmd =
      "cd " + corrupt_dir + " && tar -cf ../../" + corrupted_sp + " *";
  REQUIRE(std::system(retar_cmd.c_str()) == 0);

  // 4. Audit (should fail)
  std::string audit_corrupt_cmd = preview_path + " -a " + corrupted_sp;
  int ret = std::system(audit_corrupt_cmd.c_str());
  CHECK(ret != 0);

  fs::remove_all(test_dir);
}
