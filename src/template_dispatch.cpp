#include "encoder.h"
#include "reorder.h"
#include "util.h"
#include <array>
#include <stdexcept>
#include <string>

namespace spring {

namespace {

using template_main_fn = void (*)(const std::string &, compression_params &);

template <size_t bitset_size>
void call_reorder_main(const std::string &temp_dir, compression_params &cp) {
  reorder_main<bitset_size>(temp_dir, cp);
}

template <size_t bitset_size>
void call_encoder_main(const std::string &temp_dir, compression_params &cp) {
  encoder_main<bitset_size>(temp_dir, cp);
}

// Keep the runtime-to-template mapping explicit so supported bitset sizes stay
// easy to audit.
const std::array<template_main_fn, 16> reorder_dispatchers = {
    &call_reorder_main<64>,   &call_reorder_main<128>, &call_reorder_main<192>,
    &call_reorder_main<256>,  &call_reorder_main<320>, &call_reorder_main<384>,
    &call_reorder_main<448>,  &call_reorder_main<512>, &call_reorder_main<576>,
    &call_reorder_main<640>,  &call_reorder_main<704>, &call_reorder_main<768>,
    &call_reorder_main<832>,  &call_reorder_main<896>, &call_reorder_main<960>,
    &call_reorder_main<1024>,
};

const std::array<template_main_fn, 24> encoder_dispatchers = {
    &call_encoder_main<64>,   &call_encoder_main<128>,
    &call_encoder_main<192>,  &call_encoder_main<256>,
    &call_encoder_main<320>,  &call_encoder_main<384>,
    &call_encoder_main<448>,  &call_encoder_main<512>,
    &call_encoder_main<576>,  &call_encoder_main<640>,
    &call_encoder_main<704>,  &call_encoder_main<768>,
    &call_encoder_main<832>,  &call_encoder_main<896>,
    &call_encoder_main<960>,  &call_encoder_main<1024>,
    &call_encoder_main<1088>, &call_encoder_main<1152>,
    &call_encoder_main<1216>, &call_encoder_main<1280>,
    &call_encoder_main<1344>, &call_encoder_main<1408>,
    &call_encoder_main<1472>, &call_encoder_main<1536>,
};

size_t dispatch_index(const size_t bitset_size, const size_t max_bitset_size) {
  // Template entry points are instantiated in 64-bit increments only.
  if (bitset_size < 64 || bitset_size > max_bitset_size ||
      (bitset_size % 64) != 0) {
    throw std::runtime_error("Wrong bitset size.");
  }

  return bitset_size / 64 - 1;
}

} // namespace

void call_reorder(const std::string &temp_dir, compression_params &cp) {
  const size_t bitset_size_reorder = (2 * cp.max_readlen - 1) / 64 * 64 + 64;
  reorder_dispatchers[dispatch_index(bitset_size_reorder, 1024)](temp_dir, cp);
}

void call_encoder(const std::string &temp_dir, compression_params &cp) {
  const size_t bitset_size_encoder = (3 * cp.max_readlen - 1) / 64 * 64 + 64;
  encoder_dispatchers[dispatch_index(bitset_size_encoder, 1536)](temp_dir, cp);
}
} // namespace spring
