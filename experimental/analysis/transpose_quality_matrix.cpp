// Transposes a matrix of fixed-length quality strings so each output line holds
// one position across all reads for column-wise downstream analysis.

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct transpose_config {
  std::string input_path;
  std::string output_path;
  int read_length;
  long read_count;
};

transpose_config parse_config(char **argv) {
  transpose_config config;
  config.input_path = std::string(argv[1]);
  config.output_path = config.input_path + ".transposed";
  config.read_length = static_cast<int>(std::strtol(argv[2], nullptr, 10));
  config.read_count = std::strtol(argv[3], nullptr, 10);
  return config;
}

std::size_t quality_matrix_size(const transpose_config &config) {
  return static_cast<std::size_t>(config.read_count) *
         (config.read_length + 1);
}

std::vector<char> load_quality_array(const transpose_config &config) {
  std::vector<char> quality_matrix_data(quality_matrix_size(config));
  std::ifstream input_stream(config.input_path);
  for (long read_index = 0; read_index < config.read_count; read_index++)
    input_stream.getline(
        quality_matrix_data.data() + read_index * (config.read_length + 1),
        config.read_length + 1);
  input_stream.close();
  return quality_matrix_data;
}

void write_transposed_output(const transpose_config &config,
                             const std::vector<char> &quality_matrix_data) {
  std::ofstream output_stream(config.output_path);
  // Emit one line per read position so downstream tools can scan columns.
  for (int position_index = 0; position_index < config.read_length;
       position_index++) {
    for (long read_index = 0; read_index < config.read_count; read_index++)
      output_stream
          << quality_matrix_data[static_cast<std::size_t>(read_index) *
                                     (config.read_length + 1) +
                                 position_index];
    output_stream << "\n";
  }
  output_stream.close();
}

} // namespace

int main(int, char **argv) {
  const transpose_config config = parse_config(argv);
  const std::vector<char> quality_matrix_data = load_quality_array(config);
  write_transposed_output(config, quality_matrix_data);
  return 0;
}
