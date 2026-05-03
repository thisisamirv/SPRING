#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

#include <HuffmanCodingSymbolsPerLength.hpp>
#include <definitions.hpp>

namespace rapidgzip {

template <typename HuffmanCode, uint8_t MAX_CODE_LENGTH, typename Symbol,
          size_t MAX_SYMBOL_COUNT>
class HuffmanCodingReversedBitsCached
    : public HuffmanCodingSymbolsPerLength<HuffmanCode, MAX_CODE_LENGTH, Symbol,
                                           MAX_SYMBOL_COUNT> {
public:
  using BaseType = HuffmanCodingSymbolsPerLength<HuffmanCode, MAX_CODE_LENGTH,
                                                 Symbol, MAX_SYMBOL_COUNT>;
  using BitCount = typename BaseType::BitCount;
  using CodeLengthFrequencies = typename BaseType::CodeLengthFrequencies;

public:
  [[nodiscard]] constexpr Error
  initializeFromLengths(const VectorView<BitCount> &codeLengths) {
    if (const auto errorCode = BaseType::initializeFromLengths(codeLengths);
        errorCode != Error::NONE) {
      return errorCode;
    }

    if (m_needsToBeZeroed) {

      for (size_t symbol = 0; symbol < (1ULL << this->m_maxCodeLength);
           ++symbol) {
        m_codeCache[symbol].first = 0;
      }
    }

    auto codeValues = this->m_minimumCodeValuesPerLevel;
    for (size_t symbol = 0; symbol < codeLengths.size(); ++symbol) {
      const auto length = codeLengths[symbol];
      if (length == 0) {
        continue;
      }

      const auto k = length - this->m_minCodeLength;
      const auto code = codeValues[k]++;
      const auto reversedCode = reverseBits(code, length);

      const auto fillerBitCount = this->m_maxCodeLength - length;
      const auto maximumPaddedCode = static_cast<HuffmanCode>(
          reversedCode |
          static_cast<HuffmanCode>(nLowestBitsSet<HuffmanCode>(fillerBitCount)
                                   << length));
      assert(maximumPaddedCode < m_codeCache.size());
      const auto increment = static_cast<HuffmanCode>(HuffmanCode(1) << length);
      for (auto paddedCode = reversedCode; paddedCode <= maximumPaddedCode;
           paddedCode += increment) {
        m_codeCache[paddedCode].first = length;
        m_codeCache[paddedCode].second = static_cast<Symbol>(symbol);
      }
    }

    m_needsToBeZeroed = true;

    return Error::NONE;
  }

  [[nodiscard]] forceinline std::optional<Symbol>
  decode(gzip::BitReader &bitReader) const {
    try {
      const auto value = bitReader.peek(this->m_maxCodeLength);

      assert(value < m_codeCache.size());
      const auto [length, symbol] = m_codeCache[(int)value];

      if (length == 0) {

        return std::nullopt;
      }

      bitReader.seekAfterPeek(length);
      return symbol;
    } catch (const gzip::BitReader::EndOfFileReached &) {

      return BaseType::decode(bitReader);
    }
  }

  [[nodiscard]] constexpr auto const &codeCache() const noexcept {
    return m_codeCache;
  }

private:
  alignas(8) std::array<std::pair<uint8_t, Symbol>,
                        (1UL << MAX_CODE_LENGTH)> m_codeCache{};
  bool m_needsToBeZeroed{false};
};
} // namespace rapidgzip
