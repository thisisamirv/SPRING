#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

namespace {

struct fastq_record {
  std::string id;
  std::string read;
  std::string comment;
  std::string quality;
};

bool read_fastq_record(std::ifstream &input_stream, fastq_record &record) {
  return std::getline(input_stream, record.id) &&
         std::getline(input_stream, record.read) &&
         std::getline(input_stream, record.comment) &&
         std::getline(input_stream, record.quality);
}

bool has_matching_quality_length(const fastq_record &record) {
  return record.read.length() == record.quality.length();
}

void write_split_chunk(std::ofstream &output_stream,
                       const fastq_record &record,
                       const std::size_t chunk_start,
                       const std::size_t chunk_length) {
  output_stream << record.id << "\n"
                << record.read.substr(chunk_start, chunk_length)
                << "\n+\n"
                << record.quality.substr(chunk_start, chunk_length) << "\n";
}

void write_split_record(std::ofstream &output_stream,
                        const fastq_record &record,
                        const std::size_t max_readlen) {
  std::size_t chunk_start = 0;
  // Emit fixed-size chunks until the final tail fragment.
  while (chunk_start + max_readlen < record.read.length()) {
    write_split_chunk(output_stream, record, chunk_start, max_readlen);
    chunk_start += max_readlen;
  }

  write_split_chunk(output_stream, record, chunk_start,
                    record.read.length() - chunk_start);
}

} // namespace

int main(int, char **argv) {
  const std::string input_path = std::string(argv[1]);
  const std::size_t max_readlen =
      static_cast<std::size_t>(std::strtol(argv[2], nullptr, 10));
  const std::string output_path = input_path + ".split";
  std::ifstream input_stream(input_path);
  std::ofstream output_stream(output_path);
  fastq_record record;
  while (read_fastq_record(input_stream, record)) {
    if (!has_matching_quality_length(record)) {
      std::cout << "Quality length does not match read length\n";
      return -1;
    }

    write_split_record(output_stream, record, max_readlen);
  }
  input_stream.close();
  output_stream.close();
  return 0;
}
