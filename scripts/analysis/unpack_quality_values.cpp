#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using quality_decode_table_t = std::array<char, 4>;

struct unpack_quality_paths {
  std::string packed_input_path;
  std::string tail_input_path;
  std::string output_path;
};

quality_decode_table_t build_quality_decode_table() {
  quality_decode_table_t index_to_quality = {};
  index_to_quality[0] = 'F';
  index_to_quality[1] = ':';
  index_to_quality[2] = ',';
  index_to_quality[3] = '#';
  return index_to_quality;
}

void write_unpacked_quality(std::ofstream &output_stream,
                            uint8_t packed_quality_byte,
                            const quality_decode_table_t &index_to_quality) {
  for (int index = 0; index < 4; ++index) {
    output_stream << index_to_quality[packed_quality_byte % 4];
    packed_quality_byte /= 4;
  }
}

void unpack_quality_stream(std::ifstream &packed_input,
                           std::ofstream &output_stream,
                           const quality_decode_table_t &index_to_quality) {
  uint8_t packed_quality_byte = 0;
  while (packed_input.read(reinterpret_cast<char *>(&packed_quality_byte),
                           sizeof(uint8_t)))
    write_unpacked_quality(output_stream, packed_quality_byte,
                           index_to_quality);
}

unpack_quality_paths build_paths(char **argv) {
  const std::string input_path = std::string(argv[1]);
  return {input_path + ".packed", input_path + ".packed.tail",
          input_path + ".unpacked"};
}

void append_tail(std::ifstream &tail_input, std::ofstream &output_stream) {
  output_stream << tail_input.rdbuf();
}

} // namespace

int main(int, char **argv) {
  const unpack_quality_paths paths = build_paths(argv);
  std::ifstream packed_input(paths.packed_input_path, std::ios::binary);
  std::ifstream tail_input(paths.tail_input_path);
  std::ofstream output_stream(paths.output_path);
  const quality_decode_table_t index_to_quality =
      build_quality_decode_table();

  // Rehydrate packed bytes first, then append the leftover tail characters.
  unpack_quality_stream(packed_input, output_stream, index_to_quality);

  packed_input.close();
  append_tail(tail_input, output_stream);
  output_stream.close();
  tail_input.close();
  return 0;
}
