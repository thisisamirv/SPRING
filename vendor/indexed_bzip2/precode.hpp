#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>

#include <HuffmanCodingReversedBitsCachedCompressed.hpp>

#include "definitions.hpp"

namespace rapidgzip::deflate::precode {
constexpr auto MAX_DEPTH = MAX_PRECODE_LENGTH;

using Histogram = std::array<uint8_t, MAX_DEPTH>;

[[nodiscard]] constexpr bool operator==(const Histogram &a,
                                        const Histogram &b) {
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) {
      return false;
    }
  }
  return true;
}

template <typename Result, typename Functor, uint8_t DEPTH = 1>
constexpr void iterateValidPrecodeHistograms(
    Result &result, const Functor &processValidHistogram,
    const uint32_t remainingCount = MAX_PRECODE_COUNT,
    Histogram histogram = Histogram{}, const uint32_t freeBits = 2) {
  static_assert(DEPTH <= MAX_DEPTH,
                "Cannot descend deeper than the frequency counts!");

  for (uint32_t count = 0; count <= std::min(remainingCount, freeBits);
       ++count) {
    histogram[DEPTH - 1] = count;
    const auto newFreeBits = (freeBits - count) * 2;

    if constexpr (DEPTH == 1) {
      if (count == 1) {
        processValidHistogram(histogram);
      }
    }

    if constexpr (DEPTH == MAX_DEPTH) {
      if (newFreeBits == 0) {
        processValidHistogram(histogram);
      }
    } else {
      if (count == freeBits) {
        processValidHistogram(histogram);
      } else {
        const auto newRemainingCount = remainingCount - count;
        iterateValidPrecodeHistograms<Result, Functor, DEPTH + 1>(
            result, processValidHistogram, newRemainingCount, histogram,
            newFreeBits);
      }
    }
  }
}

inline const auto VALID_HISTOGRAMS = []() {
  std::vector<Histogram> validHistograms{};
  validHistograms.reserve(1600);

  iterateValidPrecodeHistograms(validHistograms,
                                [&validHistograms](const Histogram &histogram) {
                                  validHistograms.push_back(histogram);
                                });

#ifndef NDEBUG
  assert(validHistograms.size() == 1526);
  Histogram expected{};
  expected[0] = 2;
  assert(validHistograms.back() == expected);
#endif

  return validHistograms;
}();

inline const auto VALID_HISTOGRAMS_COUNT = VALID_HISTOGRAMS.size();
} // namespace rapidgzip::deflate::precode
