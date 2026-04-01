#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using quality_decode_table_t = std::array<char, 4>;

quality_decode_table_t build_quality_decode_table() {
  quality_decode_table_t int_to_qual = {};
  int_to_qual[0] = 'F';
  int_to_qual[1] = ':';
  int_to_qual[2] = ',';
  int_to_qual[3] = '#';
  return int_to_qual;
}

void write_unpacked_quality(std::ofstream &f_out, uint8_t quality_bin,
                            const quality_decode_table_t &int_to_qual) {
  for (int index = 0; index < 4; ++index) {
    f_out << int_to_qual[quality_bin % 4];
    quality_bin /= 4;
  }
}

void unpack_quality_stream(std::ifstream &f_in, std::ofstream &f_out,
                           const quality_decode_table_t &int_to_qual) {
  uint8_t quality_bin = 0;
  while (f_in.read(reinterpret_cast<char *>(&quality_bin), sizeof(uint8_t)))
    write_unpacked_quality(f_out, quality_bin, int_to_qual);
}

} // namespace

int main(int, char **argv) {
  const std::string infile = std::string(argv[1]);
  std::ifstream f_in(infile + ".packed", std::ios::binary);
  std::ifstream f_in_tail(infile + ".packed.tail");
  std::ofstream f_out(infile + ".unpacked");
  const quality_decode_table_t int_to_qual = build_quality_decode_table();

  unpack_quality_stream(f_in, f_out, int_to_qual);

  f_in.close();
  f_out << f_in_tail.rdbuf();
  f_out.close();
  f_in_tail.close();
  return 0;
}
