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

bool read_fastq_record(std::ifstream &f_in, fastq_record &record) {
  return std::getline(f_in, record.id) && std::getline(f_in, record.read) &&
         std::getline(f_in, record.comment) &&
         std::getline(f_in, record.quality);
}

bool has_matching_quality_length(const fastq_record &record) {
  return record.read.length() == record.quality.length();
}

void write_split_chunk(std::ofstream &f_out, const fastq_record &record,
                       const std::size_t chunk_start,
                       const std::size_t chunk_length) {
  f_out << record.id << "\n"
        << record.read.substr(chunk_start, chunk_length) << "\n+\n"
        << record.quality.substr(chunk_start, chunk_length) << "\n";
}

void write_split_record(std::ofstream &f_out, const fastq_record &record,
                        const std::size_t max_readlen) {
  std::size_t chunk_start = 0;
  while (chunk_start + max_readlen < record.read.length()) {
    write_split_chunk(f_out, record, chunk_start, max_readlen);
    chunk_start += max_readlen;
  }

  write_split_chunk(f_out, record, chunk_start,
                    record.read.length() - chunk_start);
}

} // namespace

int main(int, char **argv) {
  const std::string infile = std::string(argv[1]);
  const std::size_t max_readlen =
      static_cast<std::size_t>(std::strtol(argv[2], NULL, 10));
  const std::string outfile = infile + ".split";
  std::ifstream f_in(infile);
  std::ofstream f_out(outfile);
  fastq_record record;
  while (read_fastq_record(f_in, record)) {
    if (!has_matching_quality_length(record)) {
      std::cout << "Quality length does not match read length\n";
      return -1;
    }

    write_split_record(f_out, record, max_readlen);
  }
  f_in.close();
  f_out.close();
  return 0;
}
