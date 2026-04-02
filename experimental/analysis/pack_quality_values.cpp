// Packs a four-symbol quality alphabet into base-4 bytes, writing the packed
// stream and any leftover tail characters to companion files.

#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

namespace {

using quality_lookup_table_t = std::array<uint8_t, 128>;
using quality_chunk_t = std::array<char, 4>;

struct pack_quality_paths {
  std::string input_path;
  std::string packed_output_path;
  std::string tail_output_path;
};

quality_lookup_table_t build_quality_lookup_table() {
  quality_lookup_table_t quality_to_index = {};
  quality_to_index['F'] = 0;
  quality_to_index[':'] = 1;
  quality_to_index[','] = 2;
  quality_to_index['#'] = 3;
  return quality_to_index;
}

long input_file_length(const std::string &input_path) {
  std::ifstream input_stream(input_path, std::ios::ate);
  return input_stream.tellg();
}

uint8_t pack_quality_chunk(const quality_chunk_t &quality,
                           const quality_lookup_table_t &quality_to_index) {
  return 64 * quality_to_index[static_cast<uint8_t>(quality[3])] +
         16 * quality_to_index[static_cast<uint8_t>(quality[2])] +
         4 * quality_to_index[static_cast<uint8_t>(quality[1])] +
         quality_to_index[static_cast<uint8_t>(quality[0])];
}

void write_tail(std::ifstream &input_stream, std::ofstream &tail_output,
                const long tail_length) {
  quality_chunk_t quality = {};
  input_stream.read(quality.data(), tail_length);
  for (long tail_index = 0; tail_index < tail_length; ++tail_index)
    tail_output << quality[static_cast<size_t>(tail_index)];
}

pack_quality_paths build_paths(char **argv) {
  const std::string input_path = std::string(argv[1]);
  return {.input_path = input_path,
          .packed_output_path = input_path + ".packed",
          .tail_output_path = input_path + ".packed.tail"};
}

void write_packed_chunks(std::ifstream &input_stream,
                         std::ofstream &packed_output,
                         const long input_length,
                         const quality_lookup_table_t &quality_to_index) {
  quality_chunk_t quality = {};
  for (long chunk_index = 0; chunk_index < input_length / 4; chunk_index++) {
    input_stream.read(quality.data(), quality.size());
    const uint8_t packed_quality_byte =
        pack_quality_chunk(quality, quality_to_index);
    packed_output.write(reinterpret_cast<const char *>(&packed_quality_byte),
                        sizeof(uint8_t));
  }
}

} // namespace

int main(int, char **argv) {
  const pack_quality_paths paths = build_paths(argv);
  const quality_lookup_table_t quality_to_index =
      build_quality_lookup_table();
  const long input_length = input_file_length(paths.input_path);

  std::ifstream input_stream(paths.input_path);
  std::ofstream packed_output(paths.packed_output_path, std::ios::binary);
  std::ofstream tail_output(paths.tail_output_path);

  std::cout << input_length << "\n";
  // Pack four quality symbols into each byte and spill any remainder to tail.
  write_packed_chunks(input_stream, packed_output, input_length,
                      quality_to_index);
  packed_output.close();

  write_tail(input_stream, tail_output, input_length % 4);
  tail_output.close();
  input_stream.close();
  return 0;
}
