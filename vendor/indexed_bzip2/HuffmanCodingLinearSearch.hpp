#pragma once

#include <algorithm>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <Error.hpp>
#include <VectorView.hpp>
#include <common.hpp>

namespace rapidgzip {

template <typename T_HuffmanCode, typename T_Symbol>
class HuffmanCodingLinearSearch {
public:
  using HuffmanCode = T_HuffmanCode;
  using Symbol = T_Symbol;
  using BitCount = uint8_t;

  static constexpr auto MAX_CODE_LENGTH =
      std::numeric_limits<HuffmanCode>::digits;

  [[nodiscard]] bool isValid() const {
    return !m_codes.empty() && (m_minCodeLength > 0);
  }

  [[nodiscard]] std::vector<HuffmanCode>
  countFrequencies(const std::vector<uint8_t> &values) {
    std::vector<HuffmanCode> frequencies(std::numeric_limits<uint8_t>::max(),
                                         0);
    for (const auto value : values) {
      ++frequencies[value];
    }
    return frequencies;
  }

public:
  Error initializeFromLengths(const VectorView<BitCount> &codeLengths) {
    m_codeLengths.assign(codeLengths.begin(), codeLengths.end());
    m_minCodeLength = getMinPositive(m_codeLengths);
    m_maxCodeLength = getMax(m_codeLengths);

    static_assert(std::is_unsigned_v<HuffmanCode>,
                  "Huffman code type must be unsigned");

    const auto &bitLengths = m_codeLengths;
    auto bitLengthFrequencies = countFrequencies(bitLengths);
    if (codeLengths.size() >
        std::numeric_limits<
            std::decay_t<decltype(bitLengthFrequencies[0])>>::max()) {
      throw std::logic_error("The frequency count type must fit the count even "
                             "if all code lengths are equal!");
    }

    const auto lastNonZero =
        std::find_if(bitLengthFrequencies.rbegin(), bitLengthFrequencies.rend(),
                     [](const auto value) { return value != 0; });
    bitLengthFrequencies.resize(bitLengthFrequencies.rend() - lastNonZero);

    if (bitLengthFrequencies.empty()) {
      return Error::EMPTY_INPUT;
    }

    bitLengthFrequencies[0] = 0;

    auto maxSymbolsPossible = uint8_t(1) << bitLengthFrequencies.size();
    for (int bitLength = bitLengthFrequencies.size() - 1; bitLength > 0;
         --bitLength) {
      const auto frequency = bitLengthFrequencies.begin() + bitLength;
      if (*frequency > maxSymbolsPossible) {
        return Error::EXCEEDED_CL_LIMIT;
      }

      maxSymbolsPossible =
          (uint8_t(1) << (bitLength - 1)) - ceilDiv(*frequency, 2);
    }

    std::vector<HuffmanCode> minimumCodeValuesPerLevel(
        bitLengthFrequencies.size() + 1);
    minimumCodeValuesPerLevel[0] = 0;
    for (size_t bits = 1; bits <= bitLengthFrequencies.size(); ++bits) {
      minimumCodeValuesPerLevel[bits] =
          (minimumCodeValuesPerLevel[bits - 1] + bitLengthFrequencies[bits - 1])
          << 1U;
    }

    std::vector<HuffmanCode> huffmanCodes(bitLengths.size());
    for (size_t i = 0; i < bitLengths.size(); ++i) {
      const auto bitLength = bitLengths[i];
      if (bitLength != 0) {
        huffmanCodes[i] = minimumCodeValuesPerLevel[bitLength]++;
      }
    }

    m_codes = std::move(huffmanCodes);

#if 1
    if (m_codeLengths.size() > std::numeric_limits<Symbol>::max()) {
      throw std::invalid_argument("The range of the symbol type cannot "
                                  "represent the implied alphabet!");
    }

    for (const auto codeLength : m_codeLengths) {
      if (codeLength > std::numeric_limits<HuffmanCode>::digits) {
        std::stringstream message;
        message << "The range of the code type cannot represent the given code "
                   "lengths!\n"
                << "Got length " << static_cast<int>(codeLength)
                << " but code type width is "
                << std::numeric_limits<HuffmanCode>::digits << "\n";
        throw std::invalid_argument(std::move(message).str());
      }
    }
#endif

    return Error::NONE;
  }

  template <typename BitReader>
  [[nodiscard]] forceinline constexpr std::optional<Symbol>
  decode(BitReader &bitReader) const {
    HuffmanCode code = 0;

    for (BitCount i = 0; i < m_minCodeLength; ++i) {
      code = (code << 1U) | (bitReader.template read<1>());
    }

    for (BitCount bitLength = m_minCodeLength; bitLength <= m_maxCodeLength;
         ++bitLength) {

      for (size_t j = 0; j < m_codeLengths.size(); ++j) {
        if ((m_codeLengths[j] == bitLength) && (m_codes[j] == code)) {
          return static_cast<Symbol>(j);
        }
      }

      code <<= 1;
      code |= bitReader.template read<1>();
    }

    return std::nullopt;
  }

private:
  std::vector<BitCount> m_codeLengths;
  std::vector<HuffmanCode> m_codes;
  BitCount m_minCodeLength{0};
  BitCount m_maxCodeLength{0};
};
} // namespace rapidgzip
