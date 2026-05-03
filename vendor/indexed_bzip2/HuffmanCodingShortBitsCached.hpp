#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include <BitManipulation.hpp>
#include <HuffmanCodingSymbolsPerLength.hpp>
#include <common.hpp>

#ifndef forceinline
#if defined(_MSC_VER)
#define forceinline __forceinline
#elif defined(__clang__) || defined(__GNUC__)
#define forceinline __attribute__((always_inline)) inline
#else
#define forceinline inline
#endif
#endif

namespace rapidgzip {

template <typename HuffmanCode, uint8_t MAX_CODE_LENGTH, typename Symbol,
          size_t MAX_SYMBOL_COUNT, uint8_t LUT_BITS_COUNT, bool REVERSE_BITS,
          bool CHECK_OPTIMALITY = true>
class HuffmanCodingShortBitsCached
    : public HuffmanCodingSymbolsPerLength<HuffmanCode, MAX_CODE_LENGTH, Symbol,
                                           MAX_SYMBOL_COUNT, CHECK_OPTIMALITY> {
public:
  using BaseType =
      HuffmanCodingSymbolsPerLength<HuffmanCode, MAX_CODE_LENGTH, Symbol,
                                    MAX_SYMBOL_COUNT, CHECK_OPTIMALITY>;
  using BitCount = typename BaseType::BitCount;
  using CodeLengthFrequencies = typename BaseType::CodeLengthFrequencies;

public:
  [[nodiscard]] constexpr Error
  initializeFromLengths(const VectorView<BitCount> &codeLengths) {
    if (const auto errorCode = BaseType::initializeFromLengths(codeLengths);
        errorCode != Error::NONE) {
      return errorCode;
    }

    m_lutBitsCount = std::min(LUT_BITS_COUNT, this->m_maxCodeLength);
    m_bitsToReadAtOnce = std::max(LUT_BITS_COUNT, this->m_minCodeLength);

    if (m_needsToBeZeroed) {

      for (size_t symbol = 0; symbol < m_codeCache.size(); ++symbol) {
        m_codeCache[symbol].length = 0;
      }
    }

    auto codeValues = this->m_minimumCodeValuesPerLevel;
    for (size_t symbol = 0; symbol < codeLengths.size(); ++symbol) {
      const auto length = codeLengths[symbol];

      if ((length == 0) || (length > m_lutBitsCount)) {
        continue;
      }

      const auto fillerBitCount = static_cast<uint8_t>(m_lutBitsCount - length);
      const auto k = length - this->m_minCodeLength;
      const auto code = codeValues[k]++;
      if constexpr (REVERSE_BITS) {
        const auto reversedCode = reverseBits(code, length);
        const auto maximumPaddedCode = static_cast<HuffmanCode>(
            reversedCode |
            HuffmanCode(nLowestBitsSet<HuffmanCode>(fillerBitCount) << length));
        assert(maximumPaddedCode < m_codeCache.size());
        const auto increment =
            static_cast<HuffmanCode>(HuffmanCode(1) << length);
        for (auto paddedCode = reversedCode; paddedCode <= maximumPaddedCode;
             paddedCode += increment) {
          m_codeCache[paddedCode].length = length;
          m_codeCache[paddedCode].symbol = static_cast<Symbol>(symbol);
        }
      } else {
        const auto maximumPaddedCode = static_cast<HuffmanCode>(
            (code << fillerBitCount) |
            nLowestBitsSet<HuffmanCode>(fillerBitCount));
        assert(maximumPaddedCode < m_codeCache.size());
        for (auto paddedCode = code << fillerBitCount;
             paddedCode <= maximumPaddedCode; ++paddedCode) {
          m_codeCache[paddedCode].length = length;
          m_codeCache[paddedCode].symbol = static_cast<Symbol>(symbol);
        }
      }
    }

    m_needsToBeZeroed = true;

    return Error::NONE;
  }

  template <typename BitReader>
  [[nodiscard]] forceinline std::optional<Symbol>
  decode(BitReader &bitReader) const {
    try {
      const auto [length, symbol] = m_codeCache[bitReader.peek(m_lutBitsCount)];
      if (length == 0) {
        return decodeLong(bitReader);
      }
      bitReader.seekAfterPeek(length);
      return symbol;
    } catch (const typename BitReader::EndOfFileReached &) {

      return BaseType::decode(bitReader);
    }
  }

private:
  template <typename BitReader>
  [[nodiscard]] forceinline constexpr std::optional<Symbol>
  decodeLong(BitReader &bitReader) const {
    HuffmanCode code = 0;

    if constexpr (REVERSE_BITS) {
      for (BitCount i = 0; i < m_bitsToReadAtOnce; ++i) {
        code = HuffmanCode(code << 1U) | bitReader.template read<1>();
      }
    } else {
      code = static_cast<HuffmanCode>(bitReader.read(m_bitsToReadAtOnce));
    }

    for (BitCount k = m_bitsToReadAtOnce - this->m_minCodeLength;
         k <= this->m_maxCodeLength - this->m_minCodeLength; ++k) {
      const auto minCode = this->m_minimumCodeValuesPerLevel[k];
      if (minCode <= code) {
        const auto subIndex =
            this->m_offsets[k] + static_cast<size_t>(code - minCode);
        if (subIndex < this->m_offsets[k + 1]) {
          return this->m_symbolsPerLength[subIndex];
        }
      }

      code <<= 1;
      code |= bitReader.template read<1>();
    }

    return std::nullopt;
  }

private:
  struct CacheEntry {
    uint8_t length{0};
    Symbol symbol{0};
  };
  static_assert(sizeof(CacheEntry) == 2 * sizeof(Symbol),
                "CacheEntry is larger than assumed!");

  alignas(64) std::array<CacheEntry, (1UL << LUT_BITS_COUNT)> m_codeCache{};

  uint8_t m_lutBitsCount{LUT_BITS_COUNT};
  uint8_t m_bitsToReadAtOnce{LUT_BITS_COUNT};
  bool m_needsToBeZeroed{false};
};
} // namespace rapidgzip
