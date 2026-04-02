#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

namespace {

struct run_length_paths {
  std::string input_path;
  std::string run_length_path;
  std::string run_value_path;
};

void write_run_length(std::ofstream &run_length_output,
                      const uint16_t run_length) {
  run_length_output.write(reinterpret_cast<const char *>(&run_length),
                          sizeof(uint16_t));
}

void flush_run(std::ofstream &run_length_output,
               std::ofstream &run_value_output,
               const uint16_t run_length, const char value) {
  write_run_length(run_length_output, run_length);
  run_value_output << value;
}

run_length_paths build_paths(char **argv) {
  const std::string input_path = std::string(argv[1]);
  return {input_path, input_path + ".rl.len", input_path + ".rl.char"};
}

void encode_run_lengths(std::ifstream &input_stream,
                        std::ofstream &run_length_output,
                        std::ofstream &run_value_output) {
  char current_char;
  uint16_t run_length = 0;

  // Store F runs separately from the non-F symbols that terminate them.
  while (input_stream >> current_char) {
    if (current_char == 'F') {
      ++run_length;
      continue;
    }

    flush_run(run_length_output, run_value_output, run_length, current_char);
    run_length = 0;
  }

  if (run_length != 0)
    write_run_length(run_length_output, run_length);
}

} // namespace

int main(int, char **argv) {
  const run_length_paths paths = build_paths(argv);
  std::ifstream input_stream(paths.input_path);
  std::ofstream run_length_output(paths.run_length_path, std::ios::binary);
  std::ofstream run_value_output(paths.run_value_path);
  encode_run_lengths(input_stream, run_length_output, run_value_output);
  return 0;
}
