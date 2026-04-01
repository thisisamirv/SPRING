#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

int main(int, char **argv) {
  std::string infile = std::string(argv[1]);
  std::string outfile = infile + ".transposed";
  int readlen = static_cast<int>(std::strtol(argv[2], NULL, 10));
  long numreads = std::strtol(argv[3], NULL, 10);
  std::vector<char> quality_array(numreads * (readlen + 1));
  std::ifstream f_in(infile);
  // read file
  for (long i = 0; i < numreads; i++)
    f_in.getline(quality_array.data() + i * (readlen + 1), readlen + 1);
  f_in.close();
  // write file
  std::ofstream f_out(outfile);
  for (int j = 0; j < readlen; j++) {
    for (long i = 0; i < numreads; i++)
      f_out << quality_array[static_cast<std::size_t>(i) * (readlen + 1) + j];
    f_out << "\n";
  }
  f_out.close();
  return 0;
}
