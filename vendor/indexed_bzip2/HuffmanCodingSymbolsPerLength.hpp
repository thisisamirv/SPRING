#pragma once

#include <array>
#include <cstdint>
#include <limits>

#include <common.hpp>

#include "HuffmanCodingBase.hpp"

namespace rapidgzip {

template <typename HuffmanCode, uint8_t MAX_CODE_LENGTH, typename Symbol,
          size_t MAX_SYMBOL_COUNT, bool CHECK_OPTIMALITY = true>
class HuffmanCodingSymbolsPerLength
    : public HuffmanCodingBase<HuffmanCode, MAX_CODE_LENGTH, Symbol,
                               MAX_SYMBOL_COUNT, CHECK_OPTIMALITY> {
public:
  using BaseType = HuffmanCodingBase<HuffmanCode, MAX_CODE_LENGTH, Symbol,
                                     MAX_SYMBOL_COUNT, CHECK_OPTIMALITY>;
  using BitCount = typename BaseType::BitCount;
  using CodeLengthFrequencies = typename BaseType::CodeLengthFrequencies;

protected:
  constexpr void initializeSymbolsPerLength(
      const VectorView<BitCount> &codeLengths,
      const CodeLengthFrequencies &bitLengthFrequencies) {

    size_t sum = 0;
    for (uint8_t bitLength = this->m_minCodeLength;
         bitLength <= this->m_maxCodeLength; ++bitLength) {
      m_offsets[bitLength - this->m_minCodeLength] = static_cast<uint16_t>(sum);
      sum += bitLengthFrequencies[bitLength];
    }
    m_offsets[this->m_maxCodeLength - this->m_minCodeLength + 1] =
        static_cast<uint16_t>(sum);

    assert(sum <= m_symbolsPerLength.size() &&
           "Specified max symbol range exceeded!");
    assert(sum <= std::numeric_limits<uint16_t>::max() &&
           "Symbol count limited to 16-bit!");

    auto sizes = m_offsets;
    for (size_t symbol = 0; symbol < codeLengths.size(); ++symbol) {
      if (codeLengths[symbol] != 0) {
        const auto k = codeLengths[symbol] - this->m_minCodeLength;
        m_symbolsPerLength[sizes[k]++] = static_cast<Symbol>(symbol);
      }
    }
  }

public:
  [[nodiscard]] constexpr Error
  initializeFromLengths(const VectorView<BitCount> &codeLengths) {
    if (const auto errorCode =
            BaseType::initializeMinMaxCodeLengths(codeLengths);
        errorCode != Error::NONE) {
      return errorCode;
    }

    CodeLengthFrequencies bitLengthFrequencies = {};
    for (const auto value : codeLengths) {
      ++bitLengthFrequencies[value];
    }

    if (const auto errorCode = BaseType::checkCodeLengthFrequencies(
            bitLengthFrequencies, codeLengths.size());
        errorCode != Error::NONE) {
      return errorCode;
    }

    BaseType::initializeMinimumCodeValues(bitLengthFrequencies);

    initializeSymbolsPerLength(codeLengths, bitLengthFrequencies);

    return Error::NONE;
  }

  template <typename BitReader>
  [[nodiscard]] forceinline constexpr std::optional<Symbol>
  decode(BitReader &bitReader) const {
    HuffmanCode code = 0;

    for (BitCount i = 0; i < this->m_minCodeLength; ++i) {
      code = static_cast<HuffmanCode>(HuffmanCode(code << 1U) |
                                      bitReader.template read<1>());
    }

    for (BitCount k = 0; k <= this->m_maxCodeLength - this->m_minCodeLength;
         ++k) {
      const auto minCode = this->m_minimumCodeValuesPerLevel[k];
      if (minCode <= code) {
        const auto subIndex =
            m_offsets[k] + static_cast<size_t>(code - minCode);
        if (subIndex < m_offsets[k + 1]) {
          return m_symbolsPerLength[subIndex];
        }
      }

      code <<= 1;
      code |= bitReader.template read<1>();
    }

    return std::nullopt;
  }

protected:
  alignas(64) std::array<Symbol, MAX_SYMBOL_COUNT> m_symbolsPerLength{};

  alignas(64) std::array<uint16_t, MAX_CODE_LENGTH + 1> m_offsets{};
  static_assert(MAX_SYMBOL_COUNT + MAX_CODE_LENGTH <=
                    std::numeric_limits<uint16_t>::max(),
                "Offset type must be able to point at all symbols!");
};
} // namespace rapidgzip
