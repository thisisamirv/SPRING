// Implements DNA sequence helpers for reverse complements and packed read
// serialization used throughout compression and decompression.

#include "dna_utils.h"
#include <cstdint>
#include <stdexcept>

namespace spring {

const char chartorevchar[128] = {
    0, 0,   0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0,   0, 0, 0,
    0, 0,   0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0,   0, 0, 0,
    0, 0,   0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0,   0, 0, 'T',
    0, 'G', 0, 0, 0, 'C', 0, 0, 0, 0, 0, 0, 'N', 0, 0, 0, 0, 0, 'A', 0, 0, 0,
    0, 0,   0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0, 0,   0, 0, 0,
    0, 0,   0, 0, 0, 0,   0, 0, 0, 0, 0, 0, 0,   0, 0, 0, 0, 0};

namespace {

void fill_reverse_complement(const char *input_bases, char *output_bases,
                             const int readlen) {
  for (int i = 0; i < readlen; i++)
    output_bases[readlen - 1 - i] =
        chartorevchar[static_cast<uint8_t>(input_bases[i])];
}

template <size_t BufferSize>
void write_encoded_read(const std::string &read, std::ofstream &fout) {
  const uint32_t readlen = static_cast<uint32_t>(read.size());
  fout.write(reinterpret_cast<const char *>(&readlen), sizeof(uint32_t));
  fout.write(read.data(), static_cast<std::streamsize>(readlen));

  if (!fout.good()) {
    throw std::runtime_error("Failed writing raw read to binary stream.");
  }
}

template <size_t BufferSize>
void read_encoded_read(std::string &read, std::ifstream &fin) {
  uint32_t readlen;
  if (!fin.read(reinterpret_cast<char *>(&readlen), sizeof(uint32_t))) {
    if (fin.eof())
      return;
    throw std::runtime_error("Failed reading readlen from binary stream.");
  }
  read.resize(readlen);

  if (!fin.read(&read[0], static_cast<std::streamsize>(readlen))) {
    throw std::runtime_error("Failed reading raw DNA data from binary stream.");
  }
}

} // namespace

void reverse_complement(char *input_bases, char *output_bases,
                        const int readlen) {
  fill_reverse_complement(input_bases, output_bases, readlen);
  output_bases[readlen] = '\0';
}

std::string reverse_complement(const std::string &input_bases,
                               const int readlen) {
  std::string output_bases(readlen, '\0');
  fill_reverse_complement(input_bases.data(), &output_bases[0], readlen);
  return output_bases;
}

void write_dna_in_bits(const std::string &read, std::ofstream &fout) {
  write_encoded_read<128>(read, fout);
}

void read_dna_from_bits(std::string &read, std::ifstream &fin) {
  read_encoded_read<128>(read, fin);
}

void write_dnaN_in_bits(const std::string &read, std::ofstream &fout) {
  write_encoded_read<256>(read, fout);
}

void read_dnaN_from_bits(std::string &read, std::ifstream &fin) {
  read_encoded_read<256>(read, fin);
}

} // namespace spring
