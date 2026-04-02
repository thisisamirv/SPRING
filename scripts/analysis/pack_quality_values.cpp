#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using quality_lookup_table_t = std::array<uint8_t, 128>;
using quality_chunk_t = std::array<char, 4>;

quality_lookup_table_t build_quality_lookup_table() {
  quality_lookup_table_t qual_to_int = {};
  qual_to_int['F'] = 0;
  qual_to_int[':'] = 1;
  qual_to_int[','] = 2;
  qual_to_int['#'] = 3;
  return qual_to_int;
}

long input_file_length(const std::string &infile) {
  std::ifstream f_in(infile, std::ios::ate);
  return f_in.tellg();
}

uint8_t pack_quality_chunk(const quality_chunk_t &quality,
                           const quality_lookup_table_t &qual_to_int) {
  return 64 * qual_to_int[static_cast<uint8_t>(quality[3])] +
         16 * qual_to_int[static_cast<uint8_t>(quality[2])] +
         4 * qual_to_int[static_cast<uint8_t>(quality[1])] +
         qual_to_int[static_cast<uint8_t>(quality[0])];
}

void write_tail(std::ifstream &f_in, std::ofstream &f_out_tail,
                const long tail_length) {
  quality_chunk_t quality = {};
  f_in.read(quality.data(), tail_length);
  for (long i = 0; i < tail_length; ++i)
    f_out_tail << quality[static_cast<size_t>(i)];
}

} // namespace

int main(int, char **argv) {
  const std::string infile = std::string(argv[1]);
  const quality_lookup_table_t qual_to_int = build_quality_lookup_table();
  const long file_len = input_file_length(infile);

  std::ifstream f_in(infile);
  std::ofstream f_out(infile + ".packed", std::ios::binary);
  std::ofstream f_out_tail(infile + ".packed.tail");
  quality_chunk_t quality = {};

  std::cout << file_len << "\n";
  // Pack four quality symbols into each byte and spill any remainder to tail.
  for (long i = 0; i < file_len / 4; i++) {
    f_in.read(quality.data(), quality.size());
    const uint8_t quality_bin = pack_quality_chunk(quality, qual_to_int);
    f_out.write(reinterpret_cast<const char *>(&quality_bin), sizeof(uint8_t));
  }
  f_out.close();

  write_tail(f_in, f_out_tail, file_len % 4);
  f_out_tail.close();
  f_in.close();
  return 0;
}
