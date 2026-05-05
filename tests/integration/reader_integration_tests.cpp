#include "integration_test_support.h"

#include "common/fs_utils.h"
#include "common/params.h"
#include "decompress/archive_stream_reader.h"

#include <filesystem>
#include <sstream>

namespace fs = std::filesystem;
using namespace spring;
using namespace integration_test_support;

namespace {

TEST_CASE("SpringReader Integration Test") {
  std::string test_dir = "reader_test_tmp";
  fs::create_directories(test_dir);

  std::string input_fastq = test_dir + "/input.fastq";
  std::string archive_spring = test_dir + "/test.spring";

  const int num_records = 100;
  create_dummy_fastq(input_fastq, num_records);

  std::string compress_cmd = std::string(SPRING2_EXECUTABLE) + " -c --R1 " +
                             input_fastq + " -o " + archive_spring + " -t 1";
  REQUIRE(std::system(compress_cmd.c_str()) == 0);

  SUBCASE("Stream decompression (Single End)") {
    SpringReader reader(archive_spring, 1);

    ReadRecord rec;
    int count = 0;
    while (reader.next(rec)) {
      CHECK(rec.id == std::string("@read_") + std::to_string(count));
      count++;
    }
    CHECK(count == num_records);

    auto contents = read_files_from_tar_memory(archive_spring, {"cp.bin"});
    REQUIRE(contents.contains("cp.bin"));
    compression_params cp{};
    std::istringstream cp_input(contents["cp.bin"], std::ios::binary);
    read_compression_params(cp_input, cp);
    REQUIRE(cp_input.good());

    uint32_t seq_crc[2] = {0, 0};
    uint32_t qual_crc[2] = {0, 0};
    uint32_t id_crc[2] = {0, 0};
    reader.get_digests(seq_crc, qual_crc, id_crc);
    CHECK(seq_crc[0] == cp.read_info.sequence_crc[0]);
    CHECK(qual_crc[0] == cp.read_info.quality_crc[0]);
    CHECK(id_crc[0] == cp.read_info.id_crc[0]);
  }

  fs::remove_all(test_dir);
}

TEST_CASE("SpringReader streams grouped archives via primary read member") {
  const std::string test_dir = "reader_grouped_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string r3_fastq = test_dir + "/input_R3.fastq";
  const std::string i1_fastq = test_dir + "/input_I1.fastq";
  const std::string archive_path = test_dir + "/grouped_reader.sp";

  create_dummy_fastq(r1_fastq, 120);
  create_dummy_fastq(r2_fastq, 120);
  create_dummy_fastq(r3_fastq, 120);
  create_dummy_fastq(i1_fastq, 120);

  const std::string compress_cmd = std::string(SPRING2_EXECUTABLE) +
                                   " -c --R1 " + r1_fastq + " --R2 " +
                                   r2_fastq + " --R3 " + r3_fastq + " --I1 " +
                                   i1_fastq + " -o " + archive_path + " -t 1";
  REQUIRE(std::system(compress_cmd.c_str()) == 0);

  SpringReader reader(archive_path, 1);
  ReadRecord mate1;
  ReadRecord mate2;
  int count = 0;
  while (reader.next(mate1, mate2)) {
    CHECK(mate1.id == std::string("@read_") + std::to_string(count));
    CHECK(mate2.id == std::string("@read_") + std::to_string(count));
    count++;
  }
  CHECK(count == 120);

  fs::remove_all(test_dir);
}

} // namespace