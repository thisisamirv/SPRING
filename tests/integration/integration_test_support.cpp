#include "integration_test_support.h"

#include "common/io_utils.h"

#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace integration_test_support {

namespace fs = std::filesystem;

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
                         int read_len) {
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
                               int lf_records, int read_len) {
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
  const std::string compressed = spring::gzip_compress_string(contents, level);
  output.write(compressed.data(),
               static_cast<std::streamsize>(compressed.size()));
}

void create_dummy_fastq(const std::string &path, int num_records) {
  std::ofstream ofs(path, std::ios::binary);
  constexpr int read_len = 80;
  static constexpr char kBaseCycle[] = {'A', 'C', 'G', 'T'};
  for (int i = 0; i < num_records; ++i) {
    std::string read;
    read.reserve(read_len);
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
  static constexpr char kBaseCycle[] = {'A', 'C', 'G', 'T'};

  for (int i = 0; i < num_records; ++i) {
    const int insert_len = read_len - overlap;
    std::string read;
    read.reserve(read_len);
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
  static constexpr char kBaseCycle[] = {'A', 'C', 'G', 'T'};

  for (int i = 0; i < num_records; ++i) {
    std::string read;
    read.reserve(read_len);
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

ScopedCurrentPath::ScopedCurrentPath(const std::string &path)
    : original(fs::current_path().string()) {
  fs::current_path(path);
}

ScopedCurrentPath::~ScopedCurrentPath() {
  std::error_code ec;
  fs::current_path(original, ec);
}

} // namespace integration_test_support