#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

namespace {

void write_run_length(std::ofstream &f_out_len, const uint16_t run_length) {
  f_out_len.write(reinterpret_cast<const char *>(&run_length),
                  sizeof(uint16_t));
}

void flush_run(std::ofstream &f_out_len, std::ofstream &f_out_char,
               const uint16_t run_length, const char value) {
  write_run_length(f_out_len, run_length);
  f_out_char << value;
}

} // namespace

int main(int, char **argv) {
  const std::string infile = std::string(argv[1]);
  std::ifstream f_in(infile);
  std::ofstream f_out_len(infile + ".rl.len", std::ios::binary);
  std::ofstream f_out_char(infile + ".rl.char");
  char c;
  uint16_t run_length = 0;
  // Store F runs separately from the non-F symbols that terminate them.
  while (f_in >> c) {
    if (c == 'F') {
      ++run_length;
      continue;
    }

    flush_run(f_out_len, f_out_char, run_length, c);
    run_length = 0;
  }

  if (run_length != 0)
    f_out_len.write(reinterpret_cast<const char *>(&run_length),
                    sizeof(uint16_t));
  return 0;
}
