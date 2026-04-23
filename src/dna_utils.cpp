#include "dna_utils.h"
#include "core_utils.h"
#include <array>
#include <cstdint>
#include <stdexcept>
// DNA utility routines: conversion, reverse-complement, and small lookup
// helpers used for sequence processing and bitset packing.

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

const std::array<uint8_t, 128> &dna_to_int_lookup() {
  static const std::array<uint8_t, 128> lookup = []() {
    std::array<uint8_t, 128> table = {};
    table[(uint8_t)'A'] = 0;
    table[(uint8_t)'C'] = 2;
    table[(uint8_t)'G'] = 1;
    table[(uint8_t)'T'] = 3;
    return table;
  }();
  return lookup;
}

const std::array<uint8_t, 128> &dna_n_to_int_lookup() {
  static const std::array<uint8_t, 128> lookup = []() {
    std::array<uint8_t, 128> table = {};
    table[(uint8_t)'A'] = 0;
    table[(uint8_t)'C'] = 2;
    table[(uint8_t)'G'] = 1;
    table[(uint8_t)'T'] = 3;
    table[(uint8_t)'N'] = 4;
    return table;
  }();
  return lookup;
}

const std::array<char, 4> &int_to_dna_lookup() {
  static const std::array<char, 4> lookup = {'A', 'G', 'C', 'T'};
  return lookup;
}

const std::array<char, 5> &int_to_dna_n_lookup() {
  static const std::array<char, 5> lookup = {'A', 'G', 'C', 'T', 'N'};
  return lookup;
}

template <size_t BufferSize>
void write_encoded_read(const std::string &read, std::ofstream &fout,
                        const uint8_t *dna_to_int, const uint8_t bits_per_base,
                        const uint8_t bases_per_byte) {
  const uint32_t readlen = static_cast<uint32_t>(read.size());
  fout.write(reinterpret_cast<const char *>(&readlen), sizeof(uint32_t));
  fout.write(read.data(), static_cast<std::streamsize>(readlen));

  if (!fout.good()) {
    throw std::runtime_error("Failed writing raw read to binary stream.");
  }
}

template <size_t BufferSize>
void read_encoded_read(std::string &read, std::ifstream &fin,
                       const char *int_to_dna, const uint8_t bit_mask,
                       const uint8_t bits_per_base,
                       const uint8_t bases_per_byte) {
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
  const std::array<uint8_t, 128> &lookup = dna_to_int_lookup();
  write_encoded_read<128>(read, fout, lookup.data(), 2, 4);
}

void read_dna_from_bits(std::string &read, std::ifstream &fin) {
  const std::array<char, 4> &lookup = int_to_dna_lookup();
  read_encoded_read<128>(read, fin, lookup.data(), 3, 2, 4);
}

void write_dnaN_in_bits(const std::string &read, std::ofstream &fout) {
  const std::array<uint8_t, 128> &lookup = dna_n_to_int_lookup();
  write_encoded_read<256>(read, fout, lookup.data(), 4, 2);
}

void read_dnaN_from_bits(std::string &read, std::ifstream &fin) {
  const std::array<char, 5> &lookup = int_to_dna_n_lookup();
  read_encoded_read<256>(read, fin, lookup.data(), 15, 4, 2);
}

} // namespace spring
