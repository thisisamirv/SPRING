#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <optional>

#include <HuffmanCodingReversedCodesPerLength.hpp>
#include <definitions.hpp>

namespace rapidgzip {
template <typename HuffmanCode, uint8_t MAX_CODE_LENGTH, typename Symbol,
          size_t MAX_SYMBOL_COUNT>
class HuffmanCodingDoubleLiteralCached
    : public HuffmanCodingReversedCodesPerLength<HuffmanCode, MAX_CODE_LENGTH,
                                                 Symbol, MAX_SYMBOL_COUNT> {
public:
  using BaseType =
      HuffmanCodingReversedCodesPerLength<HuffmanCode, MAX_CODE_LENGTH, Symbol,
                                          MAX_SYMBOL_COUNT>;
  using BitCount = typename BaseType::BitCount;

  static constexpr auto LENGTH_SHIFT = 10;

  static constexpr auto NONE_SYMBOL = std::numeric_limits<Symbol>::max();
  static_assert(MAX_SYMBOL_COUNT <= NONE_SYMBOL,
                "Not enough unused symbols for special none symbol!");

public:
  constexpr Error
  initializeFromLengths(const VectorView<BitCount> &codeLengths) {

    if (const auto errorCode = BaseType::initializeFromLengths(codeLengths);
        errorCode != Error::NONE) {
      return errorCode;
    }

    if ((this->m_minCodeLength == 1) && (this->m_maxCodeLength == 1) &&
        (this->m_offsets[1] == 1)) {
      return Error::INVALID_CODE_LENGTHS;
    }

    m_nextSymbol = NONE_SYMBOL;

#if 1

    m_cachedBitCount =
        std::min<uint32_t>(std::max<uint32_t>(this->m_maxCodeLength,
                                              2 * this->m_minCodeLength + 1),
                           MAX_CODE_LENGTH);

    for (auto &x : m_doubleCodeCache) {
      x = NONE_SYMBOL;
    }

    const auto size =
        this->m_offsets[this->m_maxCodeLength - this->m_minCodeLength + 1];
    auto length = this->m_minCodeLength;
    for (size_t i = 0; i < size;) {
      const auto reversedCode = this->m_codesPerLength[i];
      const auto symbol = this->m_symbolsPerLength[i];

      if ((length + this->m_minCodeLength > m_cachedBitCount) ||
          (symbol >= 256)) {
        const auto fillerBitCount = m_cachedBitCount - length;
        const auto symbolAndLength = static_cast<Symbol>(
            symbol | (static_cast<Symbol>(length) << LENGTH_SHIFT));

        for (uint32_t fillerBits = 0;
             fillerBits < (uint32_t(1) << fillerBitCount); ++fillerBits) {
          const auto paddedCode =
              static_cast<HuffmanCode>(fillerBits << length) | reversedCode;
          m_doubleCodeCache[paddedCode * 2] = symbolAndLength;
        }
      } else {
        auto length2 = this->m_minCodeLength;
        for (size_t i2 = 0; i2 < size;) {
          const auto reversedCode2 = this->m_codesPerLength[i2];
          const auto symbol2 = this->m_symbolsPerLength[i2];

          const auto totalLength = static_cast<uint32_t>(length + length2);
          if (totalLength > m_cachedBitCount) {
            assert(length <= m_cachedBitCount);
            const auto paddedCode =
                static_cast<HuffmanCode>(
                    static_cast<HuffmanCode>(reversedCode2 << length) |
                    reversedCode) &
                nLowestBitsSet<HuffmanCode>(m_cachedBitCount);

            m_doubleCodeCache[paddedCode * 2] = static_cast<Symbol>(
                symbol | (static_cast<Symbol>(length) << LENGTH_SHIFT));

          } else {
            const auto fillerBitCount = m_cachedBitCount - totalLength;
            const auto mergedCode = static_cast<HuffmanCode>(
                (reversedCode2 << length) | reversedCode);
            const auto symbolAndLength = static_cast<Symbol>(
                symbol | (static_cast<Symbol>(totalLength) << LENGTH_SHIFT));

            for (uint32_t fillerBits = 0; fillerBits < (1U << fillerBitCount);
                 ++fillerBits) {
              const auto paddedCode =
                  static_cast<HuffmanCode>(fillerBits << totalLength) |
                  mergedCode;
              m_doubleCodeCache[paddedCode * 2] = symbolAndLength;
              m_doubleCodeCache[paddedCode * 2 + 1] = symbol2;
            }
          }

          ++i2;
          if (i2 >= size) {
            break;
          }

          while (this->m_offsets[length2 - this->m_minCodeLength + 1] == i2) {
            length2++;
          }
        }
      }

      ++i;
      if (i >= size) {
        break;
      }

      while (this->m_offsets[length - this->m_minCodeLength + 1] == i) {
        length++;
      }
    }

#else

    std::array<Symbol, 2> symbols = {NONE_SYMBOL, NONE_SYMBOL};
    size_t symbolSize = 0;

    const std::function<void(uint16_t, uint8_t)> fillCache =

        [&](uint16_t mergedCode, uint8_t mergedCodeLength) {
          assert(mergedCodeLength <= CACHED_BIT_COUNT);

          const auto emptyBitCount =
              static_cast<uint8_t>(CACHED_BIT_COUNT - mergedCodeLength);

          if ((emptyBitCount < this->m_minCodeLength) || (symbolSize >= 2) ||
              ((symbolSize > 0) && (symbols[symbolSize - 1] >= 256))) {
            for (HuffmanCode fillerBits = 1;
                 fillerBits < (1UL << emptyBitCount); ++fillerBits) {
              const auto paddedCode = static_cast<HuffmanCode>(
                  (fillerBits << mergedCodeLength) | mergedCode);

              m_doubleCodeCache[paddedCode * 2] =
                  (mergedCodeLength << LENGTH_SHIFT) | symbols[0];
              m_doubleCodeCache[paddedCode * 2 + 1] = symbols[1];
            }
            return;
          }

          symbols[symbolSize++] = 0;
          for (BitCount k = 0;
               k <= this->m_maxCodeLength - this->m_minCodeLength; ++k) {
            for (HuffmanCode subIndex = 0;
                 subIndex < this->m_offsets[k + 1] - this->m_offsets[k];
                 ++subIndex) {
              const auto code = static_cast<HuffmanCode>(
                  this->m_minimumCodeValuesPerLevel[k] + subIndex);
              const auto symbol =
                  this->m_symbolsPerLength[this->m_offsets[k] + subIndex];
              const auto length = this->m_minCodeLength + k;

              const auto reversedCode = reverseBits(code, length);

              assert((reversedCode & nLowestBitsSet<decltype(reversedCode)>(
                                         length)) == reversedCode);
              assert((mergedCode & nLowestBitsSet<decltype(mergedCode)>(
                                       mergedCodeLength)) == mergedCode);

              const auto newMergedCode =
                  static_cast<HuffmanCode>((reversedCode << mergedCodeLength) |
                                           mergedCode) &
                  nLowestBitsSet<HuffmanCode, CACHED_BIT_COUNT>();
              if (mergedCodeLength + length <= CACHED_BIT_COUNT) {
                symbols[symbolSize - 1] = symbol;
                fillCache(newMergedCode, mergedCodeLength + length);
              } else {

                m_doubleCodeCache[2 * newMergedCode] =
                    (mergedCodeLength << LENGTH_SHIFT) | symbols[0];
                m_doubleCodeCache[2 * newMergedCode + 1] = NONE_SYMBOL;
              }
            }
          }
          symbolSize--;
        };

    fillCache(0, 0);

#endif

#if 0
        std::array<uint16_t, 2 * MAX_CODE_LENGTH> mergedCodeLengthFrequencies = {};
        for ( size_t i = 0; i < m_doubleCodeCache.size(); i += 2 ) {
            mergedCodeLengthFrequencies[m_doubleCodeCache[i] >> LENGTH_SHIFT]++;
        }
        std::cerr << "Merged code length frequencies (out of " << (int)CACHED_BIT_COUNT << " cache key size):\n";
        for ( size_t i = 0; i < mergedCodeLengthFrequencies.size(); ++i ) {
            if ( mergedCodeLengthFrequencies[i] != 0 ) {
                std::cerr << " " << i << ":" << mergedCodeLengthFrequencies[i];
            }
        }
        std::cerr << "\n";
#endif

    return Error::NONE;
  }

  [[nodiscard]] forceinline std::optional<Symbol>
  decode(gzip::BitReader &bitReader) const {
    if (m_nextSymbol != NONE_SYMBOL) {
      const auto result = m_nextSymbol;
      m_nextSymbol = NONE_SYMBOL;
      return result;
    }

    try {
      const auto value = bitReader.peek(m_cachedBitCount);

      assert(value < m_doubleCodeCache.size() / 2);

      auto symbol1 = m_doubleCodeCache[(int)value * 2];
      m_nextSymbol = m_doubleCodeCache[(int)value * 2 + 1];
      assert(static_cast<uint32_t>(symbol1 >> LENGTH_SHIFT) <=
             m_cachedBitCount);
      bitReader.seekAfterPeek(symbol1 >> LENGTH_SHIFT);
      symbol1 &= nLowestBitsSet<Symbol, LENGTH_SHIFT>();

      return symbol1;
    } catch (const gzip::BitReader::EndOfFileReached &) {

      return BaseType::decode(bitReader);
    }
  }

private:
  uint32_t m_cachedBitCount{0};
  mutable Symbol m_nextSymbol = NONE_SYMBOL;

  alignas(
      8) std::array<Symbol, 2 * (1UL << MAX_CODE_LENGTH)> m_doubleCodeCache{};
};
} // namespace rapidgzip
