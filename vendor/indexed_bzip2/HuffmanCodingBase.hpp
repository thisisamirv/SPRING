#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include <Error.hpp>
#include <VectorView.hpp>
#include <common.hpp>

namespace rapidgzip {
template <typename T_HuffmanCode, uint8_t T_MAX_CODE_LENGTH, typename T_Symbol,
          size_t T_MAX_SYMBOL_COUNT, bool CHECK_OPTIMALITY = true>
class HuffmanCodingBase {
public:
  using HuffmanCode = T_HuffmanCode;
  using Symbol = T_Symbol;

  static constexpr auto MAX_CODE_LENGTH = T_MAX_CODE_LENGTH;
  static constexpr auto MAX_SYMBOL_COUNT = T_MAX_SYMBOL_COUNT;

  static_assert(MAX_CODE_LENGTH <= std::numeric_limits<HuffmanCode>::digits,
                "The huffman code type must fit the max code length!");
  static_assert(MAX_SYMBOL_COUNT <= std::numeric_limits<Symbol>::max(),
                "The symbol type must fit the highest possible symbol!");

  using BitCount = uint8_t;
  using Frequency = HuffmanCode;
  using CodeLengthFrequencies = std::array<Frequency, MAX_CODE_LENGTH + 1>;

  [[nodiscard]] bool isValid() const {
    return m_minCodeLength <= m_maxCodeLength;
  }

protected:
  constexpr Error
  initializeMinMaxCodeLengths(const VectorView<BitCount> &codeLengths) {
    static_assert(std::is_unsigned_v<HuffmanCode>,
                  "Huffman code type must be unsigned");

    if (UNLIKELY(codeLengths.empty())) [[unlikely]] {
      return Error::EMPTY_ALPHABET;
    }

    if (UNLIKELY(codeLengths.size() > MAX_SYMBOL_COUNT)) [[unlikely]] {
      throw std::invalid_argument("The range of the symbol type cannot "
                                  "represent the implied alphabet!");
    }

    m_maxCodeLength = getMax(codeLengths);

    m_minCodeLength = getMinPositive(codeLengths);
    if (UNLIKELY(m_maxCodeLength > MAX_CODE_LENGTH)) [[unlikely]] {
      throw std::invalid_argument("The range of the code type cannot represent "
                                  "the given code lengths!");
    }

    return Error::NONE;
  }

  constexpr Error
  checkCodeLengthFrequencies(const CodeLengthFrequencies &bitLengthFrequencies,
                             size_t codeLengthsSize) {

    const auto nonZeroCount = codeLengthsSize - bitLengthFrequencies[0];
    HuffmanCode unusedSymbolCount = HuffmanCode(1) << m_minCodeLength;
    for (int bitLength = m_minCodeLength; bitLength <= m_maxCodeLength;
         ++bitLength) {
      const auto frequency = bitLengthFrequencies[bitLength];
      if (frequency > unusedSymbolCount) {
        return Error::INVALID_CODE_LENGTHS;
      }
      unusedSymbolCount -= frequency;
      unusedSymbolCount *= 2;
    }

    if constexpr (CHECK_OPTIMALITY) {
      if (((nonZeroCount == 1) &&
           (unusedSymbolCount != (1U << m_maxCodeLength))) ||
          ((nonZeroCount > 1) && (unusedSymbolCount != 0))) {
        return Error::BLOATING_HUFFMAN_CODING;
      }
    }

    return Error::NONE;
  }

  constexpr void
  initializeMinimumCodeValues(CodeLengthFrequencies &bitLengthFrequencies) {

    bitLengthFrequencies[0] = 0;

    HuffmanCode minCode = 0;

    for (size_t bits = std::max<size_t>(1U, m_minCodeLength);
         bits <= m_maxCodeLength; ++bits) {
      minCode = static_cast<HuffmanCode>(
          HuffmanCode(minCode + bitLengthFrequencies[bits - 1U]) << 1U);
      m_minimumCodeValuesPerLevel[bits - m_minCodeLength] = minCode;
    }
  }

public:
  constexpr Error
  initializeFromLengths(const VectorView<BitCount> &codeLengths) {
    if (const auto errorCode = initializeMinMaxCodeLengths(codeLengths);
        errorCode != Error::NONE) {
      return errorCode;
    }

    CodeLengthFrequencies bitLengthFrequencies = {};
    for (const auto value : codeLengths) {
      ++bitLengthFrequencies[value];
    }

    if (const auto errorCode = checkCodeLengthFrequencies(bitLengthFrequencies,
                                                          codeLengths.size());
        errorCode != Error::NONE) {
      return errorCode;
    }

    initializeMinimumCodeValues(bitLengthFrequencies);

    return Error::NONE;
  }

  [[nodiscard]] constexpr BitCount minCodeLength() const noexcept {
    return m_minCodeLength;
  }

  [[nodiscard]] constexpr BitCount maxCodeLength() const noexcept {
    return m_maxCodeLength;
  }

  [[nodiscard]] constexpr auto const &
  minimumCodeValuesPerLevel() const noexcept {
    return m_minimumCodeValuesPerLevel;
  }

protected:
  BitCount m_minCodeLength{std::numeric_limits<BitCount>::max()};
  BitCount m_maxCodeLength{std::numeric_limits<BitCount>::min()};

  std::array<HuffmanCode, MAX_CODE_LENGTH + 1> m_minimumCodeValuesPerLevel{};
};

template <uint8_t MAX_CODE_LENGTH, typename CodeLengths>
[[nodiscard]] constexpr bool
checkHuffmanCodeLengths(const CodeLengths &codeLengths) {
  size_t virtualLeafCount{0};
  for (const auto codeLength : codeLengths) {
    virtualLeafCount +=
        codeLength > 0 ? 1U << (MAX_CODE_LENGTH - codeLength) : 0U;
  }

  if (virtualLeafCount == (1U << (MAX_CODE_LENGTH - 1U))) {
    size_t greaterThanOne{0};
    for (const auto codeLength : codeLengths) {
      if (codeLength > 1) {
        ++greaterThanOne;
      }
    }
    return greaterThanOne == 0;
  }
  return virtualLeafCount == (1U << MAX_CODE_LENGTH);
}
} // namespace rapidgzip
