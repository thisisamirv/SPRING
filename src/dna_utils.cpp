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
  uint8_t bitarray[BufferSize];
  uint8_t pos_in_bitarray = 0;
  const uint16_t readlen = static_cast<uint16_t>(read.size());
  fout.write(byte_ptr(&readlen), sizeof(uint16_t));

  const uint32_t full_groups = readlen / bases_per_byte;
  for (uint32_t group_index = 0; group_index < full_groups; ++group_index) {
    bitarray[pos_in_bitarray] = 0;
    for (uint8_t base_index = 0; base_index < bases_per_byte; ++base_index)
      bitarray[pos_in_bitarray] |=
          dna_to_int[static_cast<uint8_t>(
              read[bases_per_byte * group_index + base_index])]
          << (bits_per_base * base_index);
    ++pos_in_bitarray;
  }

  const uint32_t trailing_bases = readlen % bases_per_byte;
  if (trailing_bases != 0) {
    const uint32_t group_index = full_groups;
    bitarray[pos_in_bitarray] = 0;
    for (uint32_t base_index = 0; base_index < trailing_bases; ++base_index)
      bitarray[pos_in_bitarray] |=
          dna_to_int[static_cast<uint8_t>(
              read[bases_per_byte * group_index + base_index])]
          << (bits_per_base * base_index);
    ++pos_in_bitarray;
  }

  fout.write(byte_ptr(&bitarray[0]), pos_in_bitarray);
  if (!fout.good()) {
    throw std::runtime_error("Failed writing encoded read to binary stream.");
  }
}

template <size_t BufferSize>
void read_encoded_read(std::string &read, std::ifstream &fin,
                       const char *int_to_dna, const uint8_t bit_mask,
                       const uint8_t bits_per_base,
                       const uint8_t bases_per_byte) {
  uint16_t readlen;
  uint8_t bitarray[BufferSize];
  if (!fin.read(byte_ptr(&readlen), sizeof(uint16_t))) {
    if (fin.eof())
      return;
    throw std::runtime_error("Failed reading readlen from binary stream.");
  }
  read.resize(readlen);

  const uint16_t num_bytes_to_read =
      (static_cast<uint32_t>(readlen) + bases_per_byte - 1) / bases_per_byte;
  if (num_bytes_to_read > BufferSize) {
    throw std::runtime_error("Corrupted binary read: record length (" +
                             std::to_string(readlen) +
                             ") exceeds buffer capacity.");
  }
  if (!fin.read(byte_ptr(&bitarray[0]), num_bytes_to_read)) {
    throw std::runtime_error("Failed reading encoded data from binary stream.");
  }

  uint8_t pos_in_bitarray = 0;
  const uint32_t full_groups = readlen / bases_per_byte;
  for (uint32_t group_index = 0; group_index < full_groups; ++group_index) {
    for (uint32_t base_index = 0; base_index < (uint32_t)bases_per_byte;
         ++base_index) {
      read[bases_per_byte * group_index + base_index] =
          int_to_dna[bitarray[pos_in_bitarray] & bit_mask];
      bitarray[pos_in_bitarray] >>= bits_per_base;
    }
    ++pos_in_bitarray;
  }

  const uint32_t trailing_bases = readlen % bases_per_byte;
  if (trailing_bases != 0) {
    const uint32_t group_index = full_groups;
    for (uint32_t base_index = 0; base_index < trailing_bases; ++base_index) {
      read[bases_per_byte * group_index + base_index] =
          int_to_dna[bitarray[pos_in_bitarray] & bit_mask];
      bitarray[pos_in_bitarray] >>= bits_per_base;
    }
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

void write_dna_ternary_bits(const std::string &read, std::ofstream &fout) {
  const uint16_t readlen = static_cast<uint16_t>(read.size());
  fout.write(reinterpret_cast<const char *>(&readlen), sizeof(uint16_t));

  const std::array<uint8_t, 128> &lookup = dna_to_int_lookup();
  // lookup: A=0, G=1, C=2, T=3. We want A=0, G=1, T=2.
  // We'll map T(3) to 2. C(2) is our escape.

  for (uint32_t i = 0; i < readlen; i += 5) {
    uint32_t n = std::min<uint32_t>(5, readlen - i);
    bool has_c = false;
    for (uint32_t j = 0; j < n; j++) {
      if (read[i + j] == 'C' || read[i + j] == 'c') {
        has_c = true;
        break;
      }
    }

    if (!has_c) {
      uint8_t packed = 0;
      uint8_t p3 = 1;
      for (uint32_t j = 0; j < n; j++) {
        uint8_t val = lookup[static_cast<uint8_t>(read[i + j])];
        if (val == 3)
          val = 2; // T is 2 in our ternary 0,1,2
        packed += val * p3;
        p3 *= 3;
      }
      fout.put(static_cast<char>(packed));
    } else {
      // Sentinel 243
      fout.put(static_cast<char>(243));
      uint16_t escape = 0;
      for (uint32_t j = 0; j < n; j++) {
        uint8_t val = lookup[static_cast<uint8_t>(read[i + j])];
        // Use 2 bits per base for the escape block
        escape |= (val << (2 * j));
      }
      fout.write(reinterpret_cast<const char *>(&escape), sizeof(uint16_t));
    }
  }
}

void read_dna_ternary_bits(std::string &read, std::ifstream &fin) {
  uint16_t readlen;
  if (!fin.read(reinterpret_cast<char *>(&readlen), sizeof(uint16_t)))
    return;
  read.resize(readlen);

  const std::array<char, 4> &lookup = int_to_dna_lookup();
  // lookup: 0=A, 1=G, 2=C, 3=T

  for (uint32_t i = 0; i < readlen; i += 5) {
    uint32_t n = std::min<uint32_t>(5, readlen - i);
    uint8_t packed;
    if (!fin.read(reinterpret_cast<char *>(&packed), 1))
      throw std::runtime_error("Ternary read error: truncated file");

    if (packed < 243) {
      for (uint32_t j = 0; j < n; j++) {
        uint8_t val = packed % 3;
        packed /= 3;
        // Map back: 0=A, 1=G, 2=T.
        if (val == 2)
          read[i + j] = 'T';
        else
          read[i + j] = lookup[val];
      }
    } else {
      // Escape block
      uint16_t escape;
      if (!fin.read(reinterpret_cast<char *>(&escape), sizeof(uint16_t)))
        throw std::runtime_error("Ternary read error: truncated escape block");
      for (uint32_t j = 0; j < n; j++) {
        read[i + j] = lookup[(escape >> (2 * j)) & 0x03];
      }
    }
  }
}

} // namespace spring
