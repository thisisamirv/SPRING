// Provides the templated read-reordering interface.
// Implementation details are moved to reorder_impl.h to improve compilation
// efficiency.

#ifndef SPRING_REORDER_H_
#define SPRING_REORDER_H_

#include <bitset>
#include <cstdint>
#include <string>

namespace spring {

// Forward declarations to keep this header thin.
struct compression_params;
template <size_t bitset_size> struct reorder_global;
struct bbhashdict;

template <size_t bitset_size>
void bitsettostring(std::bitset<bitset_size> encoded_bases, char *read_chars,
                    const uint16_t readlen,
                    const reorder_global<bitset_size> &rg);

template <size_t bitset_size>
void setglobalarrays(reorder_global<bitset_size> &rg);

template <size_t bitset_size>
void readDnaFile(std::bitset<bitset_size> *read, uint16_t *read_lengths,
                 const reorder_global<bitset_size> &rg);

template <size_t bitset_size>
void reorder(std::bitset<bitset_size> *read, bbhashdict *dict,
             uint16_t *read_lengths, const reorder_global<bitset_size> &rg,
             const bool deterministic_mode);

template <size_t bitset_size>
void writetofile(std::bitset<bitset_size> *read, uint16_t *read_lengths,
                 reorder_global<bitset_size> &rg,
                 const bool deterministic_mode);

template <size_t bitset_size>
void reorder_main(const std::string &temp_dir, const compression_params &cp);

} // namespace spring

#endif // SPRING_REORDER_H_
