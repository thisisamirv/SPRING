#ifndef SPRING_DNA_UTILS_H_
#define SPRING_DNA_UTILS_H_

#include <fstream>
#include <string>

namespace spring {

extern const char chartorevchar[128];

void reverse_complement(char *input_bases, char *output_bases, int readlen);
std::string reverse_complement(const std::string &input_bases, int readlen);

void write_dna_in_bits(const std::string &read, std::ofstream &fout);
void read_dna_from_bits(std::string &read, std::ifstream &fin);

void write_dnaN_in_bits(const std::string &read, std::ofstream &fout);
void read_dnaN_from_bits(std::string &read, std::ifstream &fin);

} // namespace spring

#endif // SPRING_DNA_UTILS_H_
