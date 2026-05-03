#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>

#include <HuffmanCodingBase.hpp>
#include <definitions.hpp>

namespace rapidgzip {

template <typename HuffmanCode, uint8_t MAX_CODE_LENGTH, typename Symbol,
          size_t MAX_SYMBOL_COUNT>
class HuffmanCodingReversedCodesPerLength
    : public HuffmanCodingBase<HuffmanCode, MAX_CODE_LENGTH, Symbol,
                               MAX_SYMBOL_COUNT> {
public:
  using BaseType =
      HuffmanCodingBase<HuffmanCode, MAX_CODE_LENGTH, Symbol, MAX_SYMBOL_COUNT>;
  using BitCount = typename BaseType::BitCount;
  using CodeLengthFrequencies = typename BaseType::CodeLengthFrequencies;

protected:
  constexpr void
  initializeCodingTable(const VectorView<BitCount> &codeLengths,
                        const CodeLengthFrequencies &bitLengthFrequencies) {

    size_t sum = 0;
    for (uint8_t bitLength = this->m_minCodeLength;
         bitLength <= this->m_maxCodeLength; ++bitLength) {
      m_offsets[bitLength - this->m_minCodeLength] = static_cast<uint16_t>(sum);
      sum += bitLengthFrequencies[bitLength];
    }
    m_offsets[this->m_maxCodeLength - this->m_minCodeLength + 1] = sum;

    assert(sum <= m_symbolsPerLength.size() &&
           "Specified max symbol range exceeded!");

    auto sizes = m_offsets;
    auto codeValuesPerLevel = this->m_minimumCodeValuesPerLevel;
    for (size_t symbol = 0; symbol < codeLengths.size(); ++symbol) {
      const auto length = codeLengths[symbol];
      if (length != 0) {
        const auto k = length - this->m_minCodeLength;
        const auto code = codeValuesPerLevel[k];
        codeValuesPerLevel[k]++;

        m_symbolsPerLength[sizes[k]] = static_cast<Symbol>(symbol);
        m_codesPerLength[sizes[k]] = reverseBits(code, length);
        sizes[k]++;
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

    initializeCodingTable(codeLengths, bitLengthFrequencies);

    return Error::NONE;
  }

  [[nodiscard]] forceinline std::optional<Symbol>
  decode(gzip::BitReader &bitReader) const {
    HuffmanCode code = bitReader.read(this->m_minCodeLength);

    const auto size =
        m_offsets[this->m_maxCodeLength - this->m_minCodeLength + 1];
    auto relativeCodeLength = 0;
    for (size_t i = 0; i < size; ++i) {
      if (m_codesPerLength[i] == code) {
        return m_symbolsPerLength[i];
      }

      while (m_offsets[relativeCodeLength + 1] == i + 1) {
        code |= bitReader.read<1>()
                << (this->m_minCodeLength + relativeCodeLength);
        relativeCodeLength++;
      }
    }

    return std::nullopt;
  }

protected:
  alignas(8) std::array<Symbol, MAX_SYMBOL_COUNT> m_symbolsPerLength{};
  alignas(8) std::array<HuffmanCode, MAX_SYMBOL_COUNT> m_codesPerLength{};

  alignas(8) std::array<uint16_t, MAX_CODE_LENGTH + 1> m_offsets{};
  static_assert(MAX_SYMBOL_COUNT + MAX_CODE_LENGTH <=
                    std::numeric_limits<uint16_t>::max(),
                "Offset type must be able to point at all symbols!");
};
} // namespace rapidgzip
