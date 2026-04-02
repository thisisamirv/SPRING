#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

namespace {

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

} // namespace

int main(int, char **argv) {
  const std::string input_path = std::string(argv[1]);
  std::ifstream input_stream(input_path);
  std::ofstream run_length_output(input_path + ".rl.len", std::ios::binary);
  std::ofstream run_value_output(input_path + ".rl.char");
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
    run_length_output.write(reinterpret_cast<const char *>(&run_length),
                            sizeof(uint16_t));
  return 0;
}
