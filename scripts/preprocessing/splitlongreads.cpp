#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

int main(int, char **argv) {
  std::string infile = std::string(argv[1]);
  std::size_t max_readlen = static_cast<std::size_t>(std::strtol(argv[2], NULL, 10));
  std::string s[4];
  std::string outfile = infile + ".split";
  std::ifstream f_in(infile);
  std::ofstream f_out(outfile);
  while (std::getline(f_in, s[0])) {
    std::getline(f_in, s[1]);
    std::getline(f_in, s[2]);
    std::getline(f_in, s[3]);
    if (s[1].length() != s[3].length()) {
      std::cout << "Quality length does not match read length\n";
      return -1;
    }
        std::size_t chunk_start = 0;
        while (chunk_start + max_readlen < s[1].length()) {
      f_out << s[0] << "\n"
        << s[1].substr(chunk_start, max_readlen) << "\n+\n"
        << s[3].substr(chunk_start, max_readlen) << "\n";
      chunk_start += max_readlen;
    }
    // last part (only part if readlen <= max_readlen)
    f_out << s[0] << "\n"
          << s[1].substr(chunk_start, s[1].length() - chunk_start)
          << "\n+\n"
          << s[3].substr(chunk_start, s[3].length() - chunk_start)
          << "\n";
  }
  f_in.close();
  f_out.close();
  return 0;
}
