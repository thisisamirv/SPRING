#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "fs_utils.h"
#include "io_utils.h"
#include "params.h"
#include "spring_reader.h"
#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#ifndef SPRING2_EXECUTABLE
#define SPRING2_EXECUTABLE "spring2"
#endif

namespace fs = std::filesystem;
using namespace spring;

namespace {

std::string read_file_binary(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open())
    return "";
  return std::string((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
}

std::string read_gzip_file_binary(const std::string &path) {
  const std::string compressed = read_file_binary(path);
  z_stream stream{};
  stream.next_in =
      reinterpret_cast<Bytef *>(const_cast<char *>(compressed.data()));
  stream.avail_in = static_cast<uInt>(compressed.size());

  if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
    throw std::runtime_error("Failed to initialize gzip inflater.");
  }

  std::string output;
  char buffer[1 << 15];
  while (true) {
    stream.next_out = reinterpret_cast<Bytef *>(buffer);
    stream.avail_out = sizeof(buffer);

    const int status = inflate(&stream, Z_NO_FLUSH);
    const size_t produced = sizeof(buffer) - stream.avail_out;
    if (produced != 0) {
      output.append(buffer, produced);
    }

    if (status == Z_STREAM_END) {
      Bytef *next_in = stream.next_in;
      uInt avail_in = stream.avail_in;
      inflateEnd(&stream);

      while (avail_in > 0 && *next_in == 0) {
        ++next_in;
        --avail_in;
      }

      if (avail_in == 0) {
        break;
      }

      stream = {};
      stream.next_in = next_in;
      stream.avail_in = avail_in;
      if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
        throw std::runtime_error("Failed to initialize gzip inflater.");
      }
      continue;
    }

    if (status != Z_OK) {
      inflateEnd(&stream);
      throw std::runtime_error("Failed to inflate gzip data.");
    }
  }

  return output;
}

void write_fastq_record(std::ofstream &output, const std::string &id,
                        const std::string &sequence, const std::string &quality,
                        bool quality_header_has_id, bool use_crlf) {
  const char *eol = use_crlf ? "\r\n" : "\n";
  output << id << eol;
  output << sequence << eol;
  output << '+';
  if (quality_header_has_id && !id.empty() && id.front() == '@')
    output << id.substr(1);
  output << eol;
  output << quality << eol;
}

void create_custom_fastq(const std::string &path, int num_records,
                         bool quality_header_has_id, bool use_crlf,
                         int read_len = 80) {
  std::ofstream output(path, std::ios::binary);
  static constexpr char kBaseCycle[] = {'A', 'C', 'G', 'T'};
  for (int record = 0; record < num_records; ++record) {
    std::string sequence;
    sequence.reserve(static_cast<size_t>(read_len));
    for (int base = 0; base < read_len; ++base) {
      sequence.push_back(kBaseCycle[(record + base) % 4]);
    }

    write_fastq_record(output, std::string("@custom_") + std::to_string(record),
                       sequence,
                       std::string(static_cast<size_t>(read_len), 'I'),
                       quality_header_has_id, use_crlf);
  }
}

void create_late_long_fastq(const std::string &path, int short_records,
                            int total_records, int short_len, int long_len) {
  std::ofstream output(path, std::ios::binary);
  static constexpr char kBaseCycle[] = {'A', 'C', 'G', 'T'};
  for (int record = 0; record < total_records; ++record) {
    const int read_len = record < short_records ? short_len : long_len;
    std::string sequence;
    sequence.reserve(static_cast<size_t>(read_len));
    for (int base = 0; base < read_len; ++base) {
      sequence.push_back(kBaseCycle[(record + base) % 4]);
    }
    write_fastq_record(
        output, std::string("@late_long_") + std::to_string(record), sequence,
        std::string(static_cast<size_t>(read_len), 'I'), false, false);
  }
}

void create_delayed_crlf_fastq(const std::string &path, int total_records,
                               int lf_records, int read_len = 80) {
  std::ofstream output(path, std::ios::binary);
  static constexpr char kBaseCycle[] = {'A', 'C', 'G', 'T'};
  for (int record = 0; record < total_records; ++record) {
    std::string sequence;
    sequence.reserve(static_cast<size_t>(read_len));
    for (int base = 0; base < read_len; ++base) {
      sequence.push_back(kBaseCycle[(record + base) % 4]);
    }
    write_fastq_record(
        output, std::string("@delayed_crlf_") + std::to_string(record),
        sequence, std::string(static_cast<size_t>(read_len), 'I'), false,
        record >= lf_records);
  }
}

void create_custom_paired_fastqs(const std::string &r1_path,
                                 const std::string &r2_path, int num_records,
                                 bool r1_quality_header_has_id,
                                 bool r2_quality_header_has_id,
                                 bool r1_use_crlf, bool r2_use_crlf) {
  std::ofstream r1(r1_path, std::ios::binary);
  std::ofstream r2(r2_path, std::ios::binary);
  static constexpr char kBaseCycle[] = {'A', 'C', 'G', 'T'};
  constexpr int read_len = 90;

  for (int record = 0; record < num_records; ++record) {
    std::string seq1;
    std::string seq2;
    seq1.reserve(read_len);
    seq2.reserve(read_len);
    for (int base = 0; base < read_len; ++base) {
      seq1.push_back(kBaseCycle[(record + base) % 4]);
      seq2.push_back(kBaseCycle[(record + base + 1) % 4]);
    }

    write_fastq_record(
        r1, std::string("@pair_") + std::to_string(record) + "/1", seq1,
        std::string(read_len, 'I'), r1_quality_header_has_id, r1_use_crlf);
    write_fastq_record(
        r2, std::string("@pair_") + std::to_string(record) + "/2", seq2,
        std::string(read_len, 'J'), r2_quality_header_has_id, r2_use_crlf);
  }
}

void create_gzip_copy(const std::string &input_path,
                      const std::string &output_path, int level) {
  std::ifstream input(input_path, std::ios::binary);
  REQUIRE(input.is_open());

  std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
  REQUIRE(output.is_open());

  std::string contents((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
  const std::string compressed = gzip_compress_string(contents, level);
  output.write(compressed.data(),
               static_cast<std::streamsize>(compressed.size()));
  output.close();
}

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
        std::string("@scrna_") + std::to_string(record) + " 1:N:0:" + suffix;
    const std::string id2 =
        std::string("@scrna_") + std::to_string(record) + " 2:N:0:" + suffix;

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

void create_tar_with_entry(const std::string &archive_path,
                           const std::string &entry_path,
                           const std::string &contents) {
  struct archive *archive = archive_write_new();
  REQUIRE(archive != nullptr);
  REQUIRE(archive_write_set_format_pax_restricted(archive) == ARCHIVE_OK);
  REQUIRE(archive_write_open_filename(archive, archive_path.c_str()) ==
          ARCHIVE_OK);

  struct archive_entry *entry = archive_entry_new();
  REQUIRE(entry != nullptr);
  archive_entry_set_pathname(entry, entry_path.c_str());
  archive_entry_set_size(entry, static_cast<la_int64_t>(contents.size()));
  archive_entry_set_filetype(entry, AE_IFREG);
  archive_entry_set_perm(entry, 0644);
  REQUIRE(archive_write_header(archive, entry) == ARCHIVE_OK);
  REQUIRE(archive_write_data(archive, contents.data(), contents.size()) ==
          static_cast<la_ssize_t>(contents.size()));
  archive_entry_free(entry);
  REQUIRE(archive_write_close(archive) == ARCHIVE_OK);
  REQUIRE(archive_write_free(archive) == ARCHIVE_OK);
}

void replace_exact_in_file(const std::string &path, const std::string &from,
                           const std::string &to) {
  std::string contents = read_file_binary(path);
  REQUIRE(!contents.empty());
  const size_t pos = contents.find(from);
  REQUIRE(pos != std::string::npos);
  REQUIRE(contents.find(from, pos + 1) == std::string::npos);
  contents.replace(pos, from.size(), to);

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  REQUIRE(output.is_open());
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

struct ScopedCurrentPath {
  explicit ScopedCurrentPath(const fs::path &path)
      : original(fs::current_path()) {
    fs::current_path(path);
  }

  ~ScopedCurrentPath() {
    std::error_code ec;
    fs::current_path(original, ec);
  }

  fs::path original;
};

TEST_CASE("Archive Integrity Verification Test") {
  std::string test_dir = "integrity_test_tmp";
  fs::create_directories(test_dir);

  std::string input_fastq = test_dir + "/input.fastq";
  std::string archive_sp = test_dir + "/test.sp";

  int num_records = 100;
  create_dummy_fastq(input_fastq, num_records);

  // 1. Compress with mandatory audit
  std::string compress_cmd = std::string(SPRING2_EXECUTABLE) + " -c -a --R1 " +
                             input_fastq + " -o " + archive_sp + " -t 1";
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
                             input_fastq + " -o " + archive_spring + " -t 1";
  int ret = std::system(compress_cmd.c_str());
  REQUIRE(ret == 0);

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

TEST_CASE("Archive extraction rejects absolute paths") {
  const std::string test_dir = "archive_path_escape_test_tmp";
  fs::create_directories(test_dir);

  const std::string archive_path = test_dir + "/malicious.tar";
  const std::string target_dir = test_dir + "/extract";
  const std::string outside_path =
      fs::absolute(fs::path(test_dir) / "outside.txt").generic_string();

  create_tar_with_entry(archive_path, outside_path, "blocked");

  CHECK_THROWS_AS(extract_tar_archive(archive_path, target_dir),
                  std::runtime_error);
  CHECK_FALSE(fs::exists(outside_path));

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
                               input_fastq + " -o " + archive_sp + " -t " +
                               std::to_string(compress_threads);
    REQUIRE(std::system(compress_cmd.c_str()) == 0);

    // Decompress with 1 thread (different from compression thread count)
    // This validates that archive thread count is properly used internally
    std::string output_fastq =
        test_dir + "/output_t" + std::to_string(compress_threads) + ".fastq";
    std::string work_decompress =
        test_dir + "/work_decompress_t" + std::to_string(compress_threads);
    std::string decompress_cmd = std::string(SPRING2_EXECUTABLE) + " -d -i " +
                                 archive_sp + " -o " + output_fastq + " -t 1";

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

  const std::string compress_atac_cmd = std::string(SPRING2_EXECUTABLE) +
                                        " -c --R1 " + input_fastq + " -o " +
                                        archive_atac + " -t 1 -y atac";
  const std::string decompress_atac_cmd = std::string(SPRING2_EXECUTABLE) +
                                          " -d -i " + archive_atac + " -o " +
                                          output_fastq + " -t 1";
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

  const std::string compress_atac_cmd = std::string(SPRING2_EXECUTABLE) +
                                        " -c --R1 " + input_fastq + " -o " +
                                        archive_atac + " -t 1 -y atac";
  const std::string decompress_atac_cmd = std::string(SPRING2_EXECUTABLE) +
                                          " -d -i " + archive_atac + " -o " +
                                          output_fastq + " -t 1";
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

TEST_CASE("Paired FASTQ preserves per-stream plus lines and line endings") {
  const std::string test_dir = "paired_fastq_metadata_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string archive_path = test_dir + "/paired_metadata.sp";
  const std::string out_r1 = test_dir + "/roundtrip_R1.fastq";
  const std::string out_r2 = test_dir + "/roundtrip_R2.fastq";

  create_custom_paired_fastqs(r1_fastq, r2_fastq, 1500, false, true, false,
                              true);

  const std::string compress_cmd = std::string(SPRING2_EXECUTABLE) +
                                   " -c --R1 " + r1_fastq + " --R2 " +
                                   r2_fastq + " -o " + archive_path + " -t 1";
  const std::string decompress_cmd = std::string(SPRING2_EXECUTABLE) +
                                     " -d -i " + archive_path + " -o " +
                                     out_r1 + " " + out_r2 + " -t 1";

  REQUIRE(std::system(compress_cmd.c_str()) == 0);
  REQUIRE(std::system(decompress_cmd.c_str()) == 0);

  CHECK(read_file_binary(out_r1) == read_file_binary(r1_fastq));
  CHECK(read_file_binary(out_r2) == read_file_binary(r2_fastq));

  fs::remove_all(test_dir);
}

TEST_CASE("Late overlength read escalates sampled short input into long mode") {
  const std::string test_dir = "late_long_retry_test_tmp";
  fs::create_directories(test_dir);

  const std::string input_fastq = test_dir + "/input.fastq";
  const std::string archive_path = test_dir + "/late_long.sp";
  const std::string output_fastq = test_dir + "/restored.fastq";

  create_late_long_fastq(input_fastq, 10000, 10032, 80, 700);

  const std::string compress_cmd = std::string(SPRING2_EXECUTABLE) +
                                   " -c --R1 " + input_fastq + " -o " +
                                   archive_path + " -t 1";
  const std::string decompress_cmd = std::string(SPRING2_EXECUTABLE) +
                                     " -d -i " + archive_path + " -o " +
                                     output_fastq + " -t 1";

  REQUIRE(std::system(compress_cmd.c_str()) == 0);
  REQUIRE(std::system(decompress_cmd.c_str()) == 0);
  CHECK(read_file_binary(output_fastq) == read_file_binary(input_fastq));

  auto contents = read_files_from_tar_memory(archive_path, {"cp.bin"});
  REQUIRE(contents.contains("cp.bin"));
  compression_params cp{};
  std::istringstream cp_input(contents["cp.bin"], std::ios::binary);
  read_compression_params(cp_input, cp);
  REQUIRE(cp_input.good());
  CHECK(cp.encoding.long_flag);
  CHECK(cp.read_info.max_readlen == 700);

  fs::remove_all(test_dir);
}

TEST_CASE("Late CRLF updates metadata after startup sample") {
  const std::string test_dir = "late_crlf_retry_test_tmp";
  fs::create_directories(test_dir);

  const std::string input_fastq = test_dir + "/input.fastq";
  const std::string archive_path = test_dir + "/late_crlf.sp";

  create_delayed_crlf_fastq(input_fastq, 10040, 10000);

  const std::string compress_cmd = std::string(SPRING2_EXECUTABLE) +
                                   " -c --R1 " + input_fastq + " -o " +
                                   archive_path + " -t 1";
  REQUIRE(std::system(compress_cmd.c_str()) == 0);

  auto contents = read_files_from_tar_memory(archive_path, {"cp.bin"});
  REQUIRE(contents.contains("cp.bin"));
  compression_params cp{};
  std::istringstream cp_input(contents["cp.bin"], std::ios::binary);
  read_compression_params(cp_input, cp);
  REQUIRE(cp_input.good());
  CHECK(cp.encoding.use_crlf_by_stream[0]);
  CHECK(cp.encoding.use_crlf);

  fs::remove_all(test_dir);
}

TEST_CASE("Corrupt long-read archive reports a normal decompression error") {
  const std::string test_dir = "long_read_error_test_tmp";
  fs::create_directories(test_dir);

  const std::string input_fastq = test_dir + "/input.fastq";
  const std::string archive_path = test_dir + "/long_reads.sp";
  const std::string corrupt_dir = test_dir + "/corrupt_extract";
  const std::string corrupted_archive = test_dir + "/corrupted.sp";
  const std::string output_fastq = test_dir + "/restored.fastq";
  const std::string decompress_log = test_dir + "/decompress.log";
  const std::string corrupted_archive_tar_path =
      fs::absolute(corrupted_archive).generic_string();

  create_custom_fastq(input_fastq, 128, false, false, 700);

  const std::string compress_cmd = std::string(SPRING2_EXECUTABLE) +
                                   " -c --R1 " + input_fastq + " -o " +
                                   archive_path + " -t 1";
  REQUIRE(std::system(compress_cmd.c_str()) == 0);

  fs::create_directories(corrupt_dir);
  REQUIRE(std::system(
              ("tar -xf " + archive_path + " -C " + corrupt_dir).c_str()) == 0);

  const fs::path read_length_block =
      fs::path(corrupt_dir) / "readlength_1.0.bsc";
  REQUIRE(fs::exists(read_length_block));
  const auto block_size = fs::file_size(read_length_block);
  REQUIRE(block_size > 8);
  fs::resize_file(read_length_block, block_size / 2);

  REQUIRE(std::system(("cd " + corrupt_dir + " && tar -cf \"" +
                       corrupted_archive_tar_path + "\" *")
                          .c_str()) == 0);

  const std::string decompress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -d -i " + corrupted_archive + " -o " +
      output_fastq + " > " + decompress_log + " 2>&1";
  CHECK(std::system(decompress_cmd.c_str()) != 0);

  const std::string output = read_file_binary(decompress_log);
  CHECK(output.find("Program terminated unexpectedly") != std::string::npos);

  fs::remove_all(test_dir);
}

TEST_CASE("Preview and SpringReader reject truncated metadata") {
  const std::string test_dir = "truncated_metadata_preview_test_tmp";
  fs::create_directories(test_dir);

  const std::string input_fastq = test_dir + "/input.fastq";
  const std::string archive_path = test_dir + "/test.sp";
  const std::string corrupt_dir = test_dir + "/extract";
  const std::string corrupted_archive = test_dir + "/corrupted.sp";
  const std::string preview_log = test_dir + "/preview.log";
  const std::string corrupted_archive_tar_path =
      fs::absolute(corrupted_archive).generic_string();

  create_dummy_fastq(input_fastq, 200);

  const std::string compress_cmd = std::string(SPRING2_EXECUTABLE) +
                                   " -c --R1 " + input_fastq + " -o " +
                                   archive_path + " -t 1";
  REQUIRE(std::system(compress_cmd.c_str()) == 0);

  fs::create_directories(corrupt_dir);
  REQUIRE(std::system(
              ("tar -xf " + archive_path + " -C " + corrupt_dir).c_str()) == 0);

  const fs::path cp_path = fs::path(corrupt_dir) / "cp.bin";
  REQUIRE(fs::exists(cp_path));
  const auto cp_size = fs::file_size(cp_path);
  REQUIRE(cp_size > 16);
  fs::resize_file(cp_path, cp_size / 2);

  REQUIRE(std::system(("cd " + corrupt_dir + " && tar -cf \"" +
                       corrupted_archive_tar_path + "\" *")
                          .c_str()) == 0);

  const std::string preview_cmd = std::string(SPRING2_EXECUTABLE) + " -p " +
                                  corrupted_archive + " > " + preview_log +
                                  " 2>&1";
  CHECK(std::system(preview_cmd.c_str()) != 0);
  CHECK_THROWS_AS(SpringReader(corrupted_archive, 1), std::runtime_error);

  fs::remove_all(test_dir);
}

TEST_CASE("Decompression rejects colliding output paths") {
  const std::string test_dir = "decompress_output_collision_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string paired_archive = test_dir + "/paired.sp";
  const std::string single_archive = test_dir + "/single.sp";
  const std::string duplicate_output = test_dir + "/duplicate.fastq";
  const std::string duplicate_log = test_dir + "/duplicate.log";
  const std::string overwrite_log = test_dir + "/overwrite.log";

  create_dummy_fastq(r1_fastq, 180);
  create_dummy_fastq(r2_fastq, 180);

  REQUIRE(
      std::system((std::string(SPRING2_EXECUTABLE) + " -c --R1 " + r1_fastq +
                   " --R2 " + r2_fastq + " -o " + paired_archive + " -t 1")
                      .c_str()) == 0);
  REQUIRE(std::system((std::string(SPRING2_EXECUTABLE) + " -c --R1 " +
                       r1_fastq + " -o " + single_archive + " -t 1")
                          .c_str()) == 0);

  const std::string duplicate_cmd = std::string(SPRING2_EXECUTABLE) +
                                    " -d -i " + paired_archive + " -o " +
                                    duplicate_output + " " + duplicate_output +
                                    " -t 1 > " + duplicate_log + " 2>&1";
  const std::string overwrite_cmd =
      std::string(SPRING2_EXECUTABLE) + " -d -i " + single_archive + " -o " +
      single_archive + " -t 1 > " + overwrite_log + " 2>&1";

  CHECK(std::system(duplicate_cmd.c_str()) != 0);
  CHECK(std::system(overwrite_cmd.c_str()) != 0);

  fs::remove_all(test_dir);
}

TEST_CASE("Compression rejects archive paths that overwrite inputs") {
  const std::string test_dir = "compression_output_collision_test_tmp";
  fs::create_directories(test_dir);

  const std::string input_fastq = test_dir + "/input.fastq";
  const std::string collision_log = test_dir + "/collision.log";

  create_dummy_fastq(input_fastq, 160);

  const std::string compress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + input_fastq + " -o " +
      input_fastq + " -t 1 > " + collision_log + " 2>&1";
  CHECK(std::system(compress_cmd.c_str()) != 0);

  const std::string log_output = read_file_binary(collision_log);
  CHECK(log_output.find("must not overwrite an input file") !=
        std::string::npos);

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
  const std::string corrupt_preview_log = test_dir + "/corrupt_preview.log";
  const fs::path corrupted_archive_path =
      fs::absolute(fs::path(test_dir) / "grouped_corrupted.sp");

  create_dummy_fastq(r1_fastq, 500);
  create_dummy_fastq(r2_fastq, 500);
  create_dummy_fastq(r3_fastq, 500);
  create_dummy_fastq(i1_fastq, 500);

  const std::string compress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + r1_fastq + " --R2 " +
      r2_fastq + " --R3 " + r3_fastq + " --I1 " + i1_fastq +
      " -n GROUPED_TEST_NOTE -o " + archive_path + " -t 1 -y auto";
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
  const std::string corrupt_preview_cmd = std::string(SPRING2_EXECUTABLE) +
                                          " -p " + corrupted_archive_tar_path +
                                          " > " + corrupt_preview_log + " 2>&1";
  CHECK(std::system(corrupt_audit_cmd.c_str()) != 0);
  CHECK(std::system(corrupt_preview_cmd.c_str()) != 0);

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
      i2_fastq + " -o " + archive_path + " -t 1 -y sc-rna";
  const std::string decompress_cmd = std::string(SPRING2_EXECUTABLE) +
                                     " -d -i " + archive_path + " -o " +
                                     out_r1 + " " + out_r2 + " " + out_r3 +
                                     " " + out_i1 + " " + out_i2 + " -t 1";

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

TEST_CASE("Grouped decompression derives unique default output names") {
  const std::string test_dir = "grouped_default_name_test_tmp";
  fs::create_directories(test_dir + "/r1");
  fs::create_directories(test_dir + "/r2");
  fs::create_directories(test_dir + "/r3");
  fs::create_directories(test_dir + "/i1");

  const std::string r1_fastq = test_dir + "/r1/same.fastq";
  const std::string r2_fastq = test_dir + "/r2/same.fastq";
  const std::string r3_fastq = test_dir + "/r3/same.fastq";
  const std::string i1_fastq = test_dir + "/i1/same.fastq";
  const std::string archive_path =
      fs::absolute(fs::path(test_dir) / "grouped.sp").generic_string();

  create_dummy_fastq(r1_fastq, 120);
  create_dummy_fastq(r2_fastq, 120);
  create_dummy_fastq(r3_fastq, 120);
  create_dummy_fastq(i1_fastq, 120);

  const std::string compress_cmd = std::string(SPRING2_EXECUTABLE) +
                                   " -c --R1 " + r1_fastq + " --R2 " +
                                   r2_fastq + " --R3 " + r3_fastq + " --I1 " +
                                   i1_fastq + " -o " + archive_path + " -t 1";
  REQUIRE(std::system(compress_cmd.c_str()) == 0);

  {
    ScopedCurrentPath cwd_guard(test_dir);
    const std::string decompress_cmd =
        std::string(SPRING2_EXECUTABLE) + " -d -i " + archive_path + " -t 1";
    REQUIRE(std::system(decompress_cmd.c_str()) == 0);
  }

  CHECK(read_file_binary(test_dir + "/same.R1.fastq") ==
        read_file_binary(r1_fastq));
  CHECK(read_file_binary(test_dir + "/same.R2.fastq") ==
        read_file_binary(r2_fastq));
  CHECK(read_file_binary(test_dir + "/same.R3.fastq") ==
        read_file_binary(r3_fastq));
  CHECK(read_file_binary(test_dir + "/same.I1.fastq") ==
        read_file_binary(i1_fastq));

  fs::remove_all(test_dir);
}

TEST_CASE("Grouped aliased R3 output honors requested target format") {
  const std::string test_dir = "grouped_alias_format_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string archive_path = test_dir + "/grouped_alias.sp";
  const std::string out_r1 = test_dir + "/out_R1.fastq";
  const std::string out_r2 = test_dir + "/out_R2.fastq";
  const std::string out_r3 = test_dir + "/out_R3.fastq.gz";

  create_dummy_fastq(r1_fastq, 250);
  create_dummy_fastq(r2_fastq, 250);

  const std::string compress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + r1_fastq + " --R2 " +
      r2_fastq + " --R3 " + r1_fastq + " -o " + archive_path + " -t 1";
  const std::string decompress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -d -i " + archive_path + " -o " +
      out_r1 + " " + out_r2 + " " + out_r3 + " -t 1";

  REQUIRE(std::system(compress_cmd.c_str()) == 0);
  REQUIRE(std::system(decompress_cmd.c_str()) == 0);

  uint8_t flg = 0;
  uint32_t mtime = 0;
  uint8_t xfl = 0;
  uint8_t os = 0;
  std::string name;
  bool is_gzipped = false;
  bool is_bgzf = false;
  uint16_t bgzf_block_size = 0;
  uint64_t uncompressed_size = 0;
  uint64_t compressed_size = 0;
  uint32_t member_count = 0;
  extract_gzip_detailed_info(out_r3, is_gzipped, flg, mtime, xfl, os, name,
                             is_bgzf, bgzf_block_size, uncompressed_size,
                             compressed_size, member_count);
  CHECK(is_gzipped);

  const std::string restored = read_gzip_file_binary(out_r3);
  CHECK(restored == read_file_binary(r1_fastq));

  fs::remove_all(test_dir);
}

TEST_CASE("Grouped decompression rejects invalid read3 alias metadata") {
  const std::string test_dir = "grouped_invalid_alias_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string archive_path = test_dir + "/grouped_alias.sp";
  const std::string extract_dir = test_dir + "/extract";
  const std::string corrupted_archive = test_dir + "/grouped_alias_corrupt.sp";
  const std::string corrupt_log = test_dir + "/corrupt.log";
  const std::string corrupted_archive_tar_path =
      fs::absolute(corrupted_archive).generic_string();

  create_dummy_fastq(r1_fastq, 200);
  create_dummy_fastq(r2_fastq, 200);

  const std::string compress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + r1_fastq + " --R2 " +
      r2_fastq + " --R3 " + r1_fastq + " -o " + archive_path + " -t 1";
  REQUIRE(std::system(compress_cmd.c_str()) == 0);

  fs::create_directories(extract_dir);
  REQUIRE(std::system(
              ("tar -xf " + archive_path + " -C " + extract_dir).c_str()) == 0);
  replace_exact_in_file(extract_dir + "/bundle.meta", "read3_alias_source=R1",
                        "read3_alias_source=BAD");
  REQUIRE(std::system(("cd " + extract_dir + " && tar -cf \"" +
                       corrupted_archive_tar_path + "\" *")
                          .c_str()) == 0);

  const std::string decompress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -d -i " + corrupted_archive + " -o " +
      test_dir + "/out_R1.fastq " + test_dir + "/out_R2.fastq " + test_dir +
      "/out_R3.fastq -t 1 > " + corrupt_log + " 2>&1";
  CHECK(std::system(decompress_cmd.c_str()) != 0);

  fs::remove_all(test_dir);
}

TEST_CASE(
    "Grouped preview and SpringReader reject inconsistent index metadata") {
  const std::string test_dir = "grouped_invalid_index_manifest_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string i1_fastq = test_dir + "/input_I1.fastq";
  const std::string archive_path = test_dir + "/grouped_index.sp";
  const std::string extract_dir = test_dir + "/extract";
  const std::string corrupted_archive = test_dir + "/grouped_index_corrupt.sp";
  const std::string preview_log = test_dir + "/preview.log";
  const std::string corrupted_archive_tar_path =
      fs::absolute(corrupted_archive).generic_string();

  create_dummy_fastq(r1_fastq, 200);
  create_dummy_fastq(r2_fastq, 200);
  create_dummy_fastq(i1_fastq, 200);

  const std::string compress_cmd =
      std::string(SPRING2_EXECUTABLE) + " -c --R1 " + r1_fastq + " --R2 " +
      r2_fastq + " --I1 " + i1_fastq + " -o " + archive_path + " -t 1";
  REQUIRE(std::system(compress_cmd.c_str()) == 0);

  fs::create_directories(extract_dir);
  REQUIRE(std::system(
              ("tar -xf " + archive_path + " -C " + extract_dir).c_str()) == 0);
  replace_exact_in_file(extract_dir + "/bundle.meta", "has_index=1",
                        "has_index=0");
  replace_exact_in_file(extract_dir + "/bundle.meta", "has_i2=0", "has_i2=1");
  REQUIRE(std::system(("cd " + extract_dir + " && tar -cf \"" +
                       corrupted_archive_tar_path + "\" *")
                          .c_str()) == 0);

  const std::string preview_cmd = std::string(SPRING2_EXECUTABLE) + " -p " +
                                  corrupted_archive + " > " + preview_log +
                                  " 2>&1";
  CHECK(std::system(preview_cmd.c_str()) != 0);
  CHECK_THROWS_AS(SpringReader(corrupted_archive, 1), std::runtime_error);

  fs::remove_all(test_dir);
}

TEST_CASE("Paired gzip outputs preserve per-stream compression profile") {
  const std::string test_dir = "paired_gzip_profile_test_tmp";
  fs::create_directories(test_dir);

  const std::string r1_fastq = test_dir + "/input_R1.fastq";
  const std::string r2_fastq = test_dir + "/input_R2.fastq";
  const std::string archive_path = test_dir + "/paired_gzip_profile.sp";
  const std::string extract_dir = test_dir + "/archive_extract";
  const std::string baseline_r1 = test_dir + "/baseline_R1.fastq.gz";
  const std::string baseline_r2 = test_dir + "/baseline_R2.fastq.gz";
  const std::string out_r1 = test_dir + "/roundtrip_R1.fastq.gz";
  const std::string out_r2 = test_dir + "/roundtrip_R2.fastq.gz";

  create_dummy_fastq(r1_fastq, 600);
  create_dummy_fastq(r2_fastq, 600);
  create_gzip_copy(r1_fastq, baseline_r1, 1);
  create_gzip_copy(r2_fastq, baseline_r2, 9);

  const std::string compress_cmd = std::string(SPRING2_EXECUTABLE) +
                                   " -c --R1 " + r1_fastq + " --R2 " +
                                   r2_fastq + " -o " + archive_path + " -t 1";
  const std::string decompress_cmd = std::string(SPRING2_EXECUTABLE) +
                                     " -d -i " + archive_path + " -o " +
                                     out_r1 + " " + out_r2 + " -t 1";

  REQUIRE(std::system(compress_cmd.c_str()) == 0);

  fs::create_directories(extract_dir);
  REQUIRE(std::system(
              ("tar -xf " + archive_path + " -C " + extract_dir).c_str()) == 0);

  compression_params cp{};
  {
    std::ifstream cp_input(extract_dir + "/cp.bin", std::ios::binary);
    REQUIRE(cp_input.is_open());
    read_compression_params(cp_input, cp);
  }
  cp.gzip.streams[0].xfl = 4;
  cp.gzip.streams[1].xfl = 2;
  {
    std::ofstream cp_output(extract_dir + "/cp.bin",
                            std::ios::binary | std::ios::trunc);
    REQUIRE(cp_output.is_open());
    write_compression_params(cp_output, cp);
  }

  const std::string archive_tar_path =
      fs::absolute(archive_path).generic_string();
  REQUIRE(std::system(("cd " + extract_dir + " && tar -cf \"" +
                       archive_tar_path + "\" *")
                          .c_str()) == 0);

  REQUIRE(std::system(decompress_cmd.c_str()) == 0);

  CHECK(fs::file_size(baseline_r2) < fs::file_size(baseline_r1));
  CHECK(fs::file_size(out_r2) < fs::file_size(out_r1));

  const std::string restored_r1 = read_gzip_file_binary(out_r1);
  const std::string restored_r2 = read_gzip_file_binary(out_r2);
  CHECK(restored_r1 == read_file_binary(r1_fastq));
  CHECK(restored_r2 == read_file_binary(r2_fastq));

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
