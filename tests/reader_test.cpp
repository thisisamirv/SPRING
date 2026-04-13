#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "spring_reader.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace spring;

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

TEST_CASE("SpringReader Integration Test") {
  std::string test_dir = "reader_test_tmp";
  fs::create_directories(test_dir);

  std::string input_fastq = test_dir + "/input.fastq";
  std::string archive_spring = test_dir + "/test.spring";

  int num_records = 100;
  create_dummy_fastq(input_fastq, num_records);

  // Compress using the spring2 binary we just built
  std::string compress_cmd = std::string(SPRING2_EXECUTABLE) + " -c -i " +
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
      // Sequence and quality checks could be more thorough
      count++;
    }
    CHECK(count == num_records);
  }

  fs::remove_all(test_dir);
}
