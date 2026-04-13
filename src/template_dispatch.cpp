// Chooses concrete template instantiations for reorder and encoder bitset sizes
// based on the dataset's read-length requirements.

#include "encoder_impl.h"
#include "reorder_impl.h"
#include "util.h"
#include <array>
#include <stdexcept>
#include <string>

namespace spring {

namespace {

using template_main_fn = void (*)(const std::string &, compression_params &);

constexpr size_t kBitsetStep = 64;
constexpr size_t kMaxReorderBitsetSize = 1024;
constexpr size_t kMaxEncoderBitsetSize = 1536;

template <size_t bitset_size>
void call_reorder_main(const std::string &temp_dir,
                       compression_params &params) {
  reorder_main<bitset_size>(temp_dir, params);
}

template <size_t bitset_size>
void call_encoder_main(const std::string &temp_dir,
                       compression_params &params) {
  encoder_main<bitset_size>(temp_dir, params);
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

size_t rounded_bitset_size(const size_t encoded_bits_per_read) {
  return (encoded_bits_per_read - 1) / kBitsetStep * kBitsetStep + kBitsetStep;
}

size_t dispatch_index(const size_t requested_bitset_size,
                      const size_t max_supported_bitset_size) {
  // Template entry points are instantiated in 64-bit increments only.
  if (requested_bitset_size < kBitsetStep ||
      requested_bitset_size > max_supported_bitset_size ||
      (requested_bitset_size % kBitsetStep) != 0) {
    throw std::runtime_error("Wrong bitset size.");
  }

  return requested_bitset_size / kBitsetStep - 1;
}

} // namespace

void call_reorder(const std::string &temp_dir, compression_params &params) {
  const size_t reorder_bitset_size =
      rounded_bitset_size(2 * static_cast<size_t>(params.max_readlen));
  reorder_dispatchers[dispatch_index(reorder_bitset_size,
                                     kMaxReorderBitsetSize)](temp_dir, params);
}

void call_encoder(const std::string &temp_dir, compression_params &params) {
  const size_t encoder_bitset_size =
      rounded_bitset_size(3 * static_cast<size_t>(params.max_readlen));
  encoder_dispatchers[dispatch_index(encoder_bitset_size,
                                     kMaxEncoderBitsetSize)](temp_dir, params);
}
} // namespace spring
