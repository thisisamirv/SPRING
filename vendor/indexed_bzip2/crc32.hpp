#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <utility>

#ifdef LIBRAPIDARCHIVE_WITH_ISAL
#include <crc.h>
#endif

namespace rapidgzip {

using CRC32LookupTable = std::array<uint32_t, 256>;

static constexpr uint32_t CRC32_GENERATOR_POLYNOMIAL{0xEDB88320U};

[[nodiscard]] constexpr CRC32LookupTable createCRC32LookupTable() noexcept {
  CRC32LookupTable table{};
  for (uint32_t n = 0; n < table.size(); ++n) {
    auto c = static_cast<unsigned long int>(n);
    for (int j = 0; j < 8; ++j) {
      if ((c & 1UL) != 0) {
        c = CRC32_GENERATOR_POLYNOMIAL ^ (c >> 1U);
      } else {
        c >>= 1U;
      }
    }
    table[n] = c;
  }
  return table;
}

static constexpr int CRC32_LOOKUP_TABLE_SIZE = 256;

alignas(8) constexpr static CRC32LookupTable CRC32_TABLE =
    createCRC32LookupTable();

[[nodiscard]] constexpr uint32_t updateCRC32(uint32_t crc,
                                             uint8_t data) noexcept {
  return (crc >> 8U) ^ CRC32_TABLE[(crc ^ data) & 0xFFU];
}

static constexpr size_t MAX_CRC32_SLICE_SIZE = 64;

alignas(8) static constexpr std::array<
    std::array<uint32_t, 256>, MAX_CRC32_SLICE_SIZE> CRC32_SLICE_BY_N_LUT =
    []() {
      std::array<std::array<uint32_t, 256>, MAX_CRC32_SLICE_SIZE> lut{};
      lut[0] = CRC32_TABLE;
      for (size_t i = 0; i < lut[0].size(); ++i) {
        for (size_t j = 1; j < lut.size(); ++j) {
          lut[j][i] = updateCRC32(lut[j - 1][i], 0);
        }
      }
      return lut;
    }();

template <unsigned int SLICE_SIZE>
[[nodiscard]] uint32_t crc32SliceByN(uint32_t crc, const char *data,
                                     size_t size) {
  static_assert(
      SLICE_SIZE % 4 == 0,
      "Chunk size must be divisible by 4 because of the loop unrolling.");
  static_assert(SLICE_SIZE > 0, "Chunk size must not be 0.");
  static_assert(SLICE_SIZE <= MAX_CRC32_SLICE_SIZE,
                "Chunk size must not exceed the lookup table size.");

  constexpr const auto &LUT = CRC32_SLICE_BY_N_LUT;

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC unroll 8
#endif
  for (size_t i = 0; i + SLICE_SIZE <= size; i += SLICE_SIZE) {
    uint32_t firstDoubleWord{0};
    std::memcpy(&firstDoubleWord, data + i, sizeof(uint32_t));
    crc ^= firstDoubleWord;

    alignas(8) std::array<uint8_t, SLICE_SIZE> chunk{};
    std::memcpy(chunk.data(), &crc, sizeof(uint32_t));
    std::memcpy(chunk.data() + sizeof(uint32_t), data + i + sizeof(uint32_t),
                SLICE_SIZE - sizeof(uint32_t));

    uint32_t result = 0;

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC unroll 16
#endif
    for (size_t j = 0; j < SLICE_SIZE; ++j) {
      result ^= LUT[j][chunk[SLICE_SIZE - 1 - j]];
    }
    crc = result;
  }

  for (size_t i = size - (size % SLICE_SIZE); i < size; ++i) {
    crc = updateCRC32(crc, data[i]);
  }

  return crc;
}

template <unsigned int SLICE_SIZE = 16>
[[nodiscard]] uint32_t updateCRC32(const uint32_t crc, const char *const buffer,
                                   const size_t size) {
#ifdef LIBRAPIDARCHIVE_WITH_ISAL
  return ~crc32_gzip_refl(~crc, reinterpret_cast<const uint8_t *>(buffer),
                          size);
#else
  return crc32SliceByN<SLICE_SIZE>(crc, buffer, size);
#endif
}

[[nodiscard]] constexpr uint32_t
polynomialMultiplyModulo(const uint32_t a, uint32_t b, const uint32_t p) {
  uint32_t result = 0;
  for (auto coefficientPosition = uint32_t(1) << 31U; coefficientPosition > 0;
       coefficientPosition >>= 1U) {
    if ((a & coefficientPosition) != 0) {
      result ^= b;
    }

    const auto overflows = (b & 1U) != 0U;
    b >>= 1U;
    if (overflows) {
      b ^= p;
    }
  }
  return result;
}

static constexpr std::array<uint32_t, 32> X2N_LUT = []() {
  std::array<uint32_t, 32> result{};
  result[0] = uint32_t(1) << 30U;
  for (size_t n = 1; n < result.size(); ++n) {
    result[n] = polynomialMultiplyModulo(result[n - 1], result[n - 1],
                                         CRC32_GENERATOR_POLYNOMIAL);
  }
  return result;
}();

[[nodiscard]] constexpr uint32_t xPowerModulo(uint64_t exponent) {
  auto p = uint32_t(1) << 31U;
  for (size_t k = 0; exponent > 0; exponent >>= 1U, k++) {
    if ((exponent & 1U) != 0U) {
      p = polynomialMultiplyModulo(X2N_LUT[k % X2N_LUT.size()], p,
                                   CRC32_GENERATOR_POLYNOMIAL);
    }
  }
  return p;
}

[[nodiscard]] constexpr uint32_t combineCRC32(uint32_t crc1, uint32_t crc2,
                                              uint64_t crc32ByteStreamLength) {

  return polynomialMultiplyModulo(xPowerModulo(crc32ByteStreamLength * 8), crc1,
                                  CRC32_GENERATOR_POLYNOMIAL) ^
         (crc2 & 0xFFFF'FFFFU);
}

class CRC32Calculator {
public:
  void setEnabled(bool enabled) noexcept { m_enabled = enabled; }

  [[nodiscard]] constexpr bool enabled() const noexcept { return m_enabled; }

  void reset() {
    m_crc32 = ~uint32_t(0);
    m_streamSizeInBytes = 0;
  }

  [[nodiscard]] uint32_t crc32() const noexcept { return ~m_crc32; }

  [[nodiscard]] uint64_t streamSize() const noexcept {
    return m_streamSizeInBytes;
  }

  void update(const void *data, size_t size) {
    if (enabled()) {
      m_crc32 =
          updateCRC32(m_crc32, reinterpret_cast<const char *>(data), size);
      m_streamSizeInBytes += size;
    }
  }

  /**
   * Throws on error.
   */
  bool // NOLINT(modernize-use-nodiscard)
  verify(uint32_t crc32ToCompare) const {
    if (!enabled() || (crc32() == crc32ToCompare)) {
      return true;
    }

    std::stringstream message;
    message << "Mismatching CRC32 (0x" << std::hex << crc32()
            << " <-> stored: 0x" << crc32ToCompare << ")!";
    throw std::domain_error(std::move(message).str());
    return false;
  }

  void append(const CRC32Calculator &toAppend) {
    if (m_enabled != toAppend.m_enabled) {
      return;
    }
    m_crc32 = ~combineCRC32(crc32(), toAppend.crc32(), toAppend.streamSize());
    m_streamSizeInBytes += toAppend.streamSize();
  }

  void prepend(const CRC32Calculator &toPrepend) {
    if (m_enabled != toPrepend.m_enabled) {
      return;
    }
    m_crc32 = ~combineCRC32(toPrepend.crc32(), crc32(), m_streamSizeInBytes);
    m_streamSizeInBytes += toPrepend.streamSize();
  }

protected:
  uint64_t m_streamSizeInBytes{0};
  uint32_t m_crc32{~uint32_t(0)};
  bool m_enabled{true};
};

[[nodiscard]] inline uint32_t crc32(const void *const buffer,
                                    const size_t size) {
  return ~updateCRC32<>(~uint32_t(0), reinterpret_cast<const char *>(buffer),
                        size);
}
} // namespace rapidgzip
