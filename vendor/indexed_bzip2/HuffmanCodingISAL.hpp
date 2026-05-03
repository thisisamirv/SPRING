#pragma once

#include <array>
#include <cstdint>
#include <utility>

#include <Error.hpp>
#include <HuffmanCodingBase.hpp>
#include <VectorView.hpp>
#include <common.hpp>
#include <definitions.hpp>

#include <igzip_lib.h>

namespace rapidgzip::deflate {

class HuffmanCodingISAL {
public:
  static constexpr auto LIT_LEN_ELEMS = 514U;

  static constexpr auto MAX_LIT_LEN_CODE_LEN = 21U;
  static constexpr auto MAX_LIT_LEN_COUNT = MAX_LIT_LEN_CODE_LEN + 2U;
  static constexpr auto LIT_LEN = ISAL_DEF_LIT_LEN_SYMBOLS;

  static constexpr std::array<uint8_t, 32> len_extra_bit_count = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01,
      0x01, 0x02, 0x02, 0x02, 0x02, 0x03, 0x03, 0x03, 0x03, 0x04, 0x04,
      0x04, 0x04, 0x05, 0x05, 0x05, 0x05, 0x00, 0x00, 0x00, 0x00};

public:
  [[nodiscard]] Error
  initializeFromLengths(const VectorView<uint8_t> &codeLengths) {
    m_error = checkHuffmanCodeLengths<MAX_CODE_LENGTH>(codeLengths)
                  ? Error::NONE
                  : Error::INVALID_CODE_LENGTHS;

    std::array<huff_code, LIT_LEN_ELEMS> lit_and_dist_huff{};
    std::array<uint16_t, MAX_LIT_LEN_COUNT> lit_count{};
    std::array<uint16_t, MAX_LIT_LEN_COUNT> lit_expand_count{};

    for (size_t i = 0; i < codeLengths.size(); ++i) {
      const auto symbol = codeLengths[i];

      lit_count[symbol]++;
      write_huff_code(&lit_and_dist_huff[i], 0, symbol);

      if ((symbol != 0) && (i >= 264)) {
        const auto extra_count = len_extra_bit_count[i - 257U];
        lit_expand_count[symbol]--;
        lit_expand_count[symbol + extra_count] += 1U << extra_count;
      }
    }

    std::array<uint32_t, LIT_LEN_ELEMS + 2> code_list{};

    if (set_and_expand_lit_len_huffcode(
            lit_and_dist_huff.data(), LIT_LEN, lit_count.data(),
            lit_expand_count.data(), code_list.data()) != ISAL_DECOMP_OK) {
      m_error = Error::INVALID_HUFFMAN_CODE;
      return m_error;
    }

    make_inflate_huff_code_lit_len(&m_huffmanCode, lit_and_dist_huff.data(),
                                   LIT_LEN_ELEMS, lit_count.data(),
                                   code_list.data(), 0);

    return m_error;
  }

  [[nodiscard]] bool isValid() const { return m_error == Error::NONE; }

private:
  static void write_huff_code(huff_code *const huff_code, uint32_t const code,
                              uint32_t const length) {
    huff_code->code_and_length = code | (length << 24U);
  }

public:
  [[nodiscard]] forceinline std::pair<uint32_t, uint32_t>
  decode(gzip::BitReader &bitReader) const {
    static constexpr auto LARGE_SHORT_SYM_LEN = 25U;
    static constexpr auto LARGE_SHORT_SYM_MASK =
        (1U << LARGE_SHORT_SYM_LEN) - 1U;
    static constexpr auto LARGE_LONG_SYM_LEN = 10U;
    static constexpr auto LARGE_LONG_SYM_MASK = (1U << LARGE_LONG_SYM_LEN) - 1U;
    static constexpr auto LARGE_FLAG_BIT = 1U << 25U;
    static constexpr auto LARGE_SHORT_CODE_LEN_OFFSET = 28U;
    static constexpr auto LARGE_SYM_COUNT_OFFSET = 26U;
    static constexpr auto LARGE_SYM_COUNT_MASK = (1U << 2U) - 1U;
    static constexpr auto LARGE_SHORT_MAX_LEN_OFFSET = 26U;
    static constexpr auto LARGE_LONG_CODE_LEN_OFFSET = 10U;
    static constexpr auto INVALID_SYMBOL = 0x1FFFU;

    uint64_t nextBits{0};
    try {
      nextBits = bitReader.peek<32>();
    } catch (const gzip::BitReader::EndOfFileReached &exception) {

      const auto [availableBits, count] = bitReader.peekAvailable();
      if (count == 0) {
        throw exception;
      }
      nextBits = availableBits;
    }

    const auto next12Bits =
        nextBits & N_LOWEST_BITS_SET_LUT<uint64_t>[ISAL_DECODE_LONG_BITS];
    uint32_t next_sym = m_huffmanCode.short_code_lookup[next12Bits];
    if (LIKELY((next_sym & LARGE_FLAG_BIT) == 0)) [[likely]] {

      const auto bit_count = next_sym >> LARGE_SHORT_CODE_LEN_OFFSET;
      bitReader.seekAfterPeek(bit_count);

      if (bit_count == 0) {
        next_sym = INVALID_SYMBOL;
      }

      return {next_sym & LARGE_SHORT_SYM_MASK,
              (next_sym >> LARGE_SYM_COUNT_OFFSET) & LARGE_SYM_COUNT_MASK};
    }

    const auto bitCount = next_sym >> LARGE_SHORT_MAX_LEN_OFFSET;
    if (LIKELY(bitCount <= 32U)) {
      nextBits &= N_LOWEST_BITS_SET_LUT<uint64_t>[bitCount];
    } else {
      nextBits = bitReader.peek(bitCount);
    }
    next_sym =
        m_huffmanCode.long_code_lookup[(next_sym & LARGE_SHORT_SYM_MASK) +
                                       (nextBits >> ISAL_DECODE_LONG_BITS)];
    const auto bit_count = next_sym >> LARGE_LONG_CODE_LEN_OFFSET;
    bitReader.seekAfterPeek(bit_count);

    if (bit_count == 0) {
      next_sym = INVALID_SYMBOL;
    }

    return {next_sym & LARGE_LONG_SYM_MASK, 1};
  }

private:
  Error m_error{Error::INVALID_HUFFMAN_CODE};
  inflate_huff_code_large m_huffmanCode;
};
} // namespace rapidgzip::deflate
