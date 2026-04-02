#include <array>
#include <fstream>
#include <iostream>
#include <string>

namespace {

constexpr int kMaxReadLength = 512;

using read_length_counts = std::array<long, kMaxReadLength>;

bool is_sequence_line(const long line_number) { return line_number % 4 == 2; }

read_length_counts compute_read_length_distribution(
    const std::string &input_path) {
  std::ifstream input_stream(input_path);
  long line_number = 0;
  read_length_counts counts = {};
  std::string line;
  // FASTQ sequence payloads appear on every fourth line starting at line 2.
  while (std::getline(input_stream, line)) {
    ++line_number;
    if (is_sequence_line(line_number))
      ++counts[line.length()];
  }
  return counts;
}

void print_read_length_distribution(const read_length_counts &counts) {
  for (int read_length = 0; read_length < kMaxReadLength; ++read_length)
    std::cout << read_length << "\t"
              << counts[static_cast<size_t>(read_length)] << "\n";
}

} // namespace

int main(int, char **argv) {
  const std::string input_path = std::string(argv[1]);
  const read_length_counts counts =
      compute_read_length_distribution(input_path);
  print_read_length_distribution(counts);
  return 0;
}
