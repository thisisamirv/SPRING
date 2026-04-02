// Applies Illumina's standard 8-bin remapping to one quality string per line
// and writes the binned output stream for downstream compression experiments.

#include <array>
#include <cstdint>
#include <fstream>
#include <string>

namespace {

using illumina_binning_table_t = std::array<char, 128>;

struct quality_binning_paths {
  std::string input_path;
  std::string output_path;
};

illumina_binning_table_t generate_illumina_binning_table() {
  illumina_binning_table_t illumina_binning_table = {};
  for (uint8_t i = 0; i <= 33 + 1; i++)
    illumina_binning_table[i] = 33 + 0;
  for (uint8_t i = 33 + 2; i <= 33 + 9; i++)
    illumina_binning_table[i] = 33 + 6;
  for (uint8_t i = 33 + 10; i <= 33 + 19; i++)
    illumina_binning_table[i] = 33 + 15;
  for (uint8_t i = 33 + 20; i <= 33 + 24; i++)
    illumina_binning_table[i] = 33 + 22;
  for (uint8_t i = 33 + 25; i <= 33 + 29; i++)
    illumina_binning_table[i] = 33 + 27;
  for (uint8_t i = 33 + 30; i <= 33 + 34; i++)
    illumina_binning_table[i] = 33 + 33;
  for (uint8_t i = 33 + 35; i <= 33 + 39; i++)
    illumina_binning_table[i] = 33 + 37;
  for (uint8_t i = 33 + 40; i <= 127; i++)
    illumina_binning_table[i] = 33 + 40;
  return illumina_binning_table;
}

void illumina_binning(std::string &quality,
                      const illumina_binning_table_t &illumina_binning_table) {
  for (size_t quality_index = 0; quality_index < quality.length();
       quality_index++)
    quality[quality_index] =
        illumina_binning_table[static_cast<uint8_t>(quality[quality_index])];
}

quality_binning_paths parse_paths(char **argv) {
  return {.input_path = std::string(argv[1]),
          .output_path = std::string(argv[2])};
}

void bin_quality_stream(std::ifstream &input_stream,
                        std::ofstream &output_stream,
                        const illumina_binning_table_t &illumina_binning_table) {
  std::string quality;
  while (std::getline(input_stream, quality)) {
    illumina_binning(quality, illumina_binning_table);
    output_stream << quality << "\n";
  }
}

} // namespace

int main(int, char **argv) {
  // Apply the standard Illumina 8-bin remapping to each quality string.
  const illumina_binning_table_t illumina_binning_table =
      generate_illumina_binning_table();
  const quality_binning_paths paths = parse_paths(argv);
  std::ifstream input_stream(paths.input_path);
  std::ofstream output_stream(paths.output_path);
  bin_quality_stream(input_stream, output_stream, illumina_binning_table);
  input_stream.close();
  output_stream.close();
  return 0;
}
