

#pragma once

#include <array>
#include <cstdint>

#if __has_include(<BitManipulation.hpp>)
#include <BitManipulation.hpp>
#else
namespace rapidgzip {
template <typename T>
[[nodiscard]] constexpr T nLowestBitsSet(uint8_t nBitsSet) {
  if (nBitsSet == 0)
    return T(0);
  if (nBitsSet >= static_cast<uint8_t>(sizeof(T) * 8))
    return static_cast<T>(~T(0));
  return static_cast<T>((T(1) << nBitsSet) - 1);
}
template <typename T, uint8_t nBitsSet>
[[nodiscard]] constexpr T nLowestBitsSet() {
  return nLowestBitsSet<T>(nBitsSet);
}
} // namespace rapidgzip
#endif

#if __has_include(<Error.hpp>)
#include <Error.hpp>
#else
namespace rapidgzip {
enum class Error : uint8_t {
  NONE = 0,
  INVALID_CODE_LENGTHS = 1,
  BLOATING_HUFFMAN_CODING = 2
};
}
#endif

#if __has_include(<common.hpp>)
#include <common.hpp>
#else
namespace rapidgzip {
template <typename T> [[nodiscard]] constexpr T ceilDiv(T a, T b) {
  return (a + b - 1) / b;
}
} // namespace rapidgzip
#endif

#if __has_include(<definitions.hpp>)
#include <definitions.hpp>
#else
namespace rapidgzip {
namespace deflate {
constexpr uint32_t PRECODE_BITS = 3;
constexpr uint32_t MAX_PRECODE_COUNT = 19;
constexpr uint32_t MAX_PRECODE_LENGTH = (1U << PRECODE_BITS) - 1U;
} // namespace deflate
} // namespace rapidgzip
#endif

#ifndef forceinline
#if defined(_MSC_VER)
#define forceinline __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define forceinline inline __attribute__((always_inline))
#else
#define forceinline inline
#endif
#endif

namespace rapidgzip {
namespace PrecodeCheck {
namespace CountAllocatedLeaves {
using LeafCount = uint16_t;

[[nodiscard]] constexpr LeafCount
getVirtualLeafCount(const uint64_t codeLength) {

  return codeLength > 0 ? 1U << (deflate::MAX_PRECODE_LENGTH - codeLength) : 0U;
}

[[nodiscard]] constexpr LeafCount
getVirtualLeafCount(const uint64_t precodeBits, const size_t codeLengthCount) {
  size_t virtualLeafCount{0};
  for (size_t i = 0; i < codeLengthCount; ++i) {
    virtualLeafCount +=
        getVirtualLeafCount((precodeBits >> (i * deflate::PRECODE_BITS)) &
                            nLowestBitsSet<uint64_t, deflate::PRECODE_BITS>());
  }
  return virtualLeafCount;
}

template <uint8_t VALUE_BITS, uint8_t VALUE_COUNT>
[[nodiscard]] constexpr LeafCount computeLeafCount(uint64_t values) {
  LeafCount result{0};
  for (uint8_t i = 0; i < VALUE_COUNT; ++i) {
    result += getVirtualLeafCount((values >> (i * VALUE_BITS)) &
                                  nLowestBitsSet<LeafCount, VALUE_BITS>());
  }
  return result;
}

template <uint8_t PRECODE_CHUNK_SIZE>
[[nodiscard]] static const std::array<
    LeafCount, 1ULL << uint16_t(PRECODE_CHUNK_SIZE * deflate::PRECODE_BITS)> &
getPrecodeToLeafCountLUT() {
  using LUT_t = std::array<LeafCount, 1ULL << uint16_t(PRECODE_CHUNK_SIZE *
                                                       deflate::PRECODE_BITS)>;
  static const LUT_t lut = []() {
    LUT_t result{};
    for (size_t i = 0; i < result.size(); ++i) {
      result[i] =
          computeLeafCount<deflate::PRECODE_BITS, PRECODE_CHUNK_SIZE>(i);
    }
    return result;
  }();
  return lut;
}

template <uint8_t PRECODE_CHUNK_SIZE>
[[nodiscard]] forceinline LeafCount precodesToLeafCount(uint64_t precodeBits) {
  constexpr auto CACHED_BITS = deflate::PRECODE_BITS * PRECODE_CHUNK_SIZE;
  constexpr auto CHUNK_COUNT =
      ceilDiv(deflate::MAX_PRECODE_COUNT, PRECODE_CHUNK_SIZE);
  LeafCount histogram{0};
  for (size_t chunk = 0; chunk < CHUNK_COUNT; ++chunk) {
    auto precodeChunk = precodeBits >> (chunk * CACHED_BITS);

    if (chunk != CHUNK_COUNT - 1) {
      precodeChunk &= nLowestBitsSet<uint64_t, CACHED_BITS>();
    }
    histogram += getPrecodeToLeafCountLUT<PRECODE_CHUNK_SIZE>()[precodeChunk];
  }
  return histogram;
}

[[nodiscard]] inline Error checkPrecode(const uint64_t next4Bits,
                                        const uint64_t next57Bits) {
  const auto codeLengthCount = 4 + next4Bits;
  constexpr auto PRECODE_CHUNK_SIZE = 4U;
#if 0
    const auto precodeBits = next57Bits & nLowestBitsSet<uint64_t>( codeLengthCount * deflate::PRECODE_BITS );

    constexpr auto CACHED_BITS = deflate::PRECODE_BITS * PRECODE_CHUNK_SIZE;
    const auto CHUNK_COUNT = ceilDiv( codeLengthCount, PRECODE_CHUNK_SIZE );
    LeafCount virtualLeafCount{ 0 };
    
    for ( size_t chunk = 0; chunk < CHUNK_COUNT; ++chunk ) {
        auto precodeChunk = precodeBits >> ( chunk * CACHED_BITS );
        

        if ( chunk != CHUNK_COUNT - 1 ) {
            precodeChunk &= nLowestBitsSet<uint64_t, CACHED_BITS>();
        }
        virtualLeafCount += PRECODE_TO_LEAF_COUNT_LUT<PRECODE_CHUNK_SIZE>[precodeChunk];
    }
#elif 1

  const auto &LUT = getPrecodeToLeafCountLUT<PRECODE_CHUNK_SIZE>();
  constexpr auto CACHED_BITS = deflate::PRECODE_BITS * PRECODE_CHUNK_SIZE;
  const auto precodeBits =
      next57Bits &
      nLowestBitsSet<uint64_t>(codeLengthCount * deflate::PRECODE_BITS);

  LeafCount virtualLeafCount{0};
  virtualLeafCount +=
      LUT[precodeBits & nLowestBitsSet<uint64_t, CACHED_BITS>()];
  virtualLeafCount += LUT[(precodeBits >> CACHED_BITS) &
                          nLowestBitsSet<uint64_t, CACHED_BITS>()];

  virtualLeafCount += LUT[(precodeBits >> (2U * CACHED_BITS)) &
                          nLowestBitsSet<uint64_t, CACHED_BITS>()];
  virtualLeafCount += LUT[(precodeBits >> (3U * CACHED_BITS)) &
                          nLowestBitsSet<uint64_t, CACHED_BITS>()];

  virtualLeafCount += LUT[precodeBits >> (4U * CACHED_BITS)];

#elif 0

  const auto precodeBits =
      next57Bits &
      nLowestBitsSet<uint64_t>(codeLengthCount * deflate::PRECODE_BITS);
  const auto virtualLeafCount =
      precodesToLeafCount<PRECODE_CHUNK_SIZE>(precodeBits);
#else

  const auto virtualLeafCount =
      getVirtualLeafCount(next57Bits, codeLengthCount);
#endif

  if (virtualLeafCount == 64) {
    return Error::NONE;
  }
  return virtualLeafCount == 128 ? Error::NONE : Error::INVALID_CODE_LENGTHS;

  if (virtualLeafCount > 128) {
    return Error::INVALID_CODE_LENGTHS;
  }
  if (virtualLeafCount < 128) {
    return Error::BLOATING_HUFFMAN_CODING;
  }
  return Error::NONE;
}
} // namespace CountAllocatedLeaves
} // namespace PrecodeCheck
} // namespace rapidgzip
