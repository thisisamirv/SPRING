#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace rapidgzip {
[[nodiscard]] inline bool isLittleEndian() {
  constexpr uint16_t endianTestNumber = 1;
  return *reinterpret_cast<const uint8_t *>(&endianTestNumber) == 1;
}

[[nodiscard]] constexpr uint64_t byteSwap(uint64_t value) {
  value = ((value & uint64_t(0x0000'0000'FFFF'FFFFULL)) << 32U) |
          ((value & uint64_t(0xFFFF'FFFF'0000'0000ULL)) >> 32U);
  value = ((value & uint64_t(0x0000'FFFF'0000'FFFFULL)) << 16U) |
          ((value & uint64_t(0xFFFF'0000'FFFF'0000ULL)) >> 16U);
  value = ((value & uint64_t(0x00FF'00FF'00FF'00FFULL)) << 8U) |
          ((value & uint64_t(0xFF00'FF00'FF00'FF00ULL)) >> 8U);
  return value;
}

[[nodiscard]] constexpr uint32_t byteSwap(uint32_t value) {
  value = ((value & uint32_t(0x0000'FFFFUL)) << 16U) |
          ((value & uint32_t(0xFFFF'0000UL)) >> 16U);
  value = ((value & uint32_t(0x00FF'00FFUL)) << 8U) |
          ((value & uint32_t(0xFF00'FF00UL)) >> 8U);
  return value;
}

[[nodiscard]] constexpr uint16_t byteSwap(uint16_t value) {
  return static_cast<uint16_t>(
      ((static_cast<uint32_t>(value) & 0x00FFU) << 8U) |
      ((static_cast<uint32_t>(value) & 0xFF00U) >> 8U));
}

template <typename T>
[[nodiscard]] constexpr T nLowestBitsSet(uint8_t nBitsSet) {
  static_assert(std::is_unsigned_v<T>, "Type must be unsigned!");
  if (nBitsSet == 0) {
    return T(0);
  }
  if (nBitsSet >= std::numeric_limits<T>::digits) {
    return static_cast<T>(~T(0));
  }
  const auto nZeroBits = static_cast<uint8_t>(
      std::max(0, std::numeric_limits<T>::digits - nBitsSet));
  return static_cast<T>(static_cast<T>(~T(0)) >> nZeroBits);
}

template <typename T, uint8_t nBitsSet>
[[nodiscard]] constexpr T nLowestBitsSet() {
  static_assert(std::is_unsigned_v<T>, "Type must be unsigned!");
  if constexpr (nBitsSet == 0) {
    return T(0);
  } else if constexpr (nBitsSet >= std::numeric_limits<T>::digits) {
    return static_cast<T>(~T(0));
  } else {
    const auto nZeroBits = static_cast<uint8_t>(
        std::max(0, std::numeric_limits<T>::digits - nBitsSet));
    return static_cast<T>(static_cast<T>(~T(0)) >> nZeroBits);
  }
}

template <typename T>
static constexpr std::array<T, 256U> N_LOWEST_BITS_SET_LUT = []() {
  std::array<T, 256U> result{};
  for (size_t i = 0; i < result.size(); ++i) {
    result[i] = nLowestBitsSet<T>(static_cast<uint8_t>(i));
  }
  return result;
}();

template <typename T>
[[nodiscard]] constexpr T nHighestBitsSet(uint8_t nBitsSet) {
  static_assert(std::is_unsigned_v<T>, "Type must be unsigned!");
  if (nBitsSet == 0) {
    return T(0);
  }
  if (nBitsSet >= std::numeric_limits<T>::digits) {
    return static_cast<T>(~T(0));
  }
  const auto nZeroBits = static_cast<uint8_t>(
      std::max(0, std::numeric_limits<T>::digits - nBitsSet));
  return static_cast<T>(static_cast<T>(~T(0)) << nZeroBits);
}

template <typename T>
static constexpr std::array<T, 256U> N_HIGHEST_BITS_SET_LUT = []() {
  std::array<T, 256U> result{};
  for (size_t i = 0; i < result.size(); ++i) {
    result[i] = nHighestBitsSet<T>(i);
  }
  return result;
}();

template <typename T, uint8_t nBitsSet>
[[nodiscard]] constexpr T nHighestBitsSet() {
  static_assert(std::is_unsigned_v<T>, "Type must be unsigned!");
  if constexpr (nBitsSet == 0) {
    return T(0);
  } else if constexpr (nBitsSet >= std::numeric_limits<T>::digits) {
    return static_cast<T>(~T(0));
  } else {
    const auto nZeroBits =
        std::max(0, std::numeric_limits<T>::digits - nBitsSet);
    return static_cast<T>(static_cast<T>(~T(0)) << nZeroBits);
  }
}

[[nodiscard]] constexpr uint8_t reverseBitsWithoutLUT(uint8_t data) {

  constexpr std::array<uint8_t, 3> masks = {0b0101'0101U, 0b0011'0011U,
                                            0b0000'1111U};
  for (size_t i = 0; i < masks.size(); ++i) {
    const uint32_t mask{masks[i]};
    data = static_cast<uint8_t>(((uint32_t(data) & mask) << (1U << i)) |
                                ((uint32_t(data) & ~mask) >> (1U << i)));
  }
  return data;
}

[[nodiscard]] constexpr uint16_t reverseBitsWithoutLUT(uint16_t data) {
  /* Reverse bits using bit-parallelism in a recursive fashion, i.e., first swap
   * every bit with its neighbor, then swap each half nibble with its neighbor,
   * then each nibble, then each byte. */
  constexpr std::array<uint16_t, 4> masks = {
      0b0101'0101'0101'0101U,
      0b0011'0011'0011'0011U,
      0b0000'1111'0000'1111U,
      0b0000'0000'1111'1111U,
  };
  for (size_t i = 0; i < masks.size(); ++i) {
    const uint32_t mask{masks[i]};
    data = static_cast<uint16_t>(((uint32_t(data) & mask) << (1U << i)) |
                                 ((uint32_t(data) & ~mask) >> (1U << i)));
  }
  return data;
}

[[nodiscard]] constexpr uint32_t reverseBitsWithoutLUT(uint32_t data) {
  /* Reverse bits using bit-parallelism in a recursive fashion, i.e., first swap
   * every bit with its neighbor, then swap each half nibble with its neighbor,
   * then each nibble, then each byte, then each word. */
  constexpr std::array<uint32_t, 5> masks = {
      0b0101'0101'0101'0101'0101'0101'0101'0101U,
      0b0011'0011'0011'0011'0011'0011'0011'0011U,
      0b0000'1111'0000'1111'0000'1111'0000'1111U,
      0b0000'0000'1111'1111'0000'0000'1111'1111U,
      0b0000'0000'0000'0000'1111'1111'1111'1111U,
  };
  for (size_t i = 0; i < masks.size(); ++i) {
    data = ((data & masks[i]) << (1U << i)) | ((data & ~masks[i]) >> (1U << i));
  }
  return data;
}

[[nodiscard]] constexpr uint64_t reverseBitsWithoutLUT(uint64_t data) {

  constexpr std::array<uint64_t, 6> masks = {
      0b0101'0101'0101'0101'0101'0101'0101'0101'0101'0101'0101'0101'0101'0101'0101'0101U,
      0b0011'0011'0011'0011'0011'0011'0011'0011'0011'0011'0011'0011'0011'0011'0011'0011U,
      0b0000'1111'0000'1111'0000'1111'0000'1111'0000'1111'0000'1111'0000'1111'0000'1111U,
      0b0000'0000'1111'1111'0000'0000'1111'1111'0000'0000'1111'1111'0000'0000'1111'1111U,
      0b0000'0000'0000'0000'1111'1111'1111'1111'0000'0000'0000'0000'1111'1111'1111'1111U,
      0b0000'0000'0000'0000'0000'0000'0000'0000'1111'1111'1111'1111'1111'1111'1111'1111U,
  };

  for (size_t i = 0; i < masks.size(); ++i) {
    data = ((data & masks[i]) << (1U << i)) | ((data & ~masks[i]) >> (1U << i));
  }
  return data;
}

template <typename T>
[[nodiscard]] constexpr std::array<
    T, 1ULL << static_cast<uint8_t>(std::numeric_limits<T>::digits)>
createReversedBitsLUT();

template <>
[[nodiscard]] constexpr std::array<uint8_t, 1ULL << 8U>
createReversedBitsLUT<uint8_t>() {
  using T = uint8_t;
  static_assert(std::is_unsigned_v<T> && std::is_integral_v<T>);

  std::array<T, 1ULL << static_cast<uint8_t>(std::numeric_limits<T>::digits)>
      result{};
  for (size_t i = 0; i < result.size(); ++i) {
    result[i] = reverseBitsWithoutLUT(static_cast<T>(i));
  }
  return result;
}

template <>
[[nodiscard]] constexpr std::array<uint16_t, 1ULL << 16U>
createReversedBitsLUT<uint16_t>() {
  using T = uint16_t;
  static_assert(std::is_unsigned_v<T> && std::is_integral_v<T>);

  constexpr auto REVERSED_BITS_LUT = createReversedBitsLUT<uint8_t>();

  std::array<T, 1ULL << static_cast<uint8_t>(std::numeric_limits<T>::digits)>
      result{};
  for (size_t i = 0; i < result.size(); ++i) {
    result[i] = (REVERSED_BITS_LUT[i & 0xFFU] << 8U) |
                REVERSED_BITS_LUT[(i >> 8U) & 0xFFU];
  }
  return result;
}

template <typename T> [[nodiscard]] inline const auto &reversedBitsLUT() {
  alignas(8) static const auto lut = createReversedBitsLUT<T>();
  return lut;
}

template <typename T> [[nodiscard]] constexpr T reverseBits(T value) {
  static_assert(std::is_unsigned_v<T> && std::is_integral_v<T>);

  if constexpr (sizeof(T) <= 2) {
    if (std::is_constant_evaluated()) {
      return reverseBitsWithoutLUT(value);
    }
    return reversedBitsLUT<T>()[value];
  } else {
    return reverseBitsWithoutLUT(value);
  }
}

template <typename T>
[[nodiscard]] constexpr T reverseBits(T value, uint8_t bitCount) {
  return reverseBits<T>(value) >>
         static_cast<uint8_t>(std::numeric_limits<T>::digits - bitCount);
}

[[nodiscard]] constexpr uint8_t requiredBits(const uint64_t stateCount) {
  if (stateCount == 0) {
    return 0;
  }
  if (stateCount == 1) {
    return 1;
  }

  uint8_t result{0};
  for (auto maxValue = stateCount - 1; maxValue != 0; maxValue >>= 1U) {
    ++result;
  }
  return result;
}
} // namespace rapidgzip
