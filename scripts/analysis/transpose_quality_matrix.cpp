#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct transpose_config {
  std::string infile;
  std::string outfile;
  int readlen;
  long numreads;
};

transpose_config parse_config(char **argv) {
  transpose_config config;
  config.infile = std::string(argv[1]);
  config.outfile = config.infile + ".transposed";
  config.readlen = static_cast<int>(std::strtol(argv[2], NULL, 10));
  config.numreads = std::strtol(argv[3], NULL, 10);
  return config;
}

std::vector<char> load_quality_array(const transpose_config &config) {
  std::vector<char> quality_array(static_cast<std::size_t>(config.numreads) *
                                  (config.readlen + 1));
  std::ifstream f_in(config.infile);
  for (long i = 0; i < config.numreads; i++)
    f_in.getline(quality_array.data() + i * (config.readlen + 1),
                 config.readlen + 1);
  f_in.close();
  return quality_array;
}

void write_transposed_output(const transpose_config &config,
                             const std::vector<char> &quality_array) {
  std::ofstream f_out(config.outfile);
  // Emit one line per read position so downstream tools can scan columns.
  for (int j = 0; j < config.readlen; j++) {
    for (long i = 0; i < config.numreads; i++)
      f_out << quality_array[static_cast<std::size_t>(i) *
                                 (config.readlen + 1) +
                             j];
    f_out << "\n";
  }
  f_out.close();
}

} // namespace

int main(int, char **argv) {
  const transpose_config config = parse_config(argv);
  const std::vector<char> quality_array = load_quality_array(config);
  write_transposed_output(config, quality_array);
  return 0;
}
