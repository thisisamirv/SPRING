#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include <Error.hpp>
#include <VectorView.hpp>
#include <common.hpp>
#include <definitions.hpp>

#include <igzip_lib.h>

namespace rapidgzip {

class HuffmanCodingDistanceISAL {
public:
  static constexpr auto DIST_LEN = ISAL_DEF_DIST_SYMBOLS;
  static constexpr auto LIT_LEN = ISAL_DEF_LIT_LEN_SYMBOLS;
  static constexpr auto LIT_LEN_ELEMS = 514U;

public:
  [[nodiscard]] Error
  initializeFromLengths(const VectorView<uint8_t> &codeLengths) {
    std::array<huff_code, LIT_LEN_ELEMS> dist_huff{};
    std::array<uint16_t, 16> dist_count{};

    for (size_t i = 0; i < codeLengths.size(); ++i) {
      const auto symbol = codeLengths[i];

      dist_count[symbol]++;
      write_huff_code(&dist_huff[i], 0, symbol);
    }

    if (set_codes(dist_huff.data(), LIT_LEN, dist_count.data()) !=
        ISAL_DECOMP_OK) {
      m_error = Error::INVALID_HUFFMAN_CODE;
      return m_error;
    }

    make_inflate_huff_code_dist(&m_huffmanCode, dist_huff.data(), DIST_LEN,
                                dist_count.data(), DIST_LEN);

    m_error = Error::NONE;
    return Error::NONE;
  }

  [[nodiscard]] bool isValid() const { return m_error == Error::NONE; }

private:
  static void write_huff_code(huff_code *const huff_code, uint32_t const code,
                              uint32_t const length) {
    huff_code->code_and_length = code | (length << 24U);
  }

public:
  [[nodiscard]] forceinline std::optional<uint16_t>
  decode(gzip::BitReader &bitReader) const {
    static constexpr auto SMALL_SHORT_SYM_LEN = 9U;
    static constexpr auto SMALL_SHORT_SYM_MASK =
        ((1U << SMALL_SHORT_SYM_LEN) - 1U);
    static constexpr auto SMALL_SHORT_CODE_LEN_OFFSET = 11U;
    static constexpr auto SMALL_LONG_CODE_LEN_OFFSET = 10U;
    static constexpr auto SMALL_FLAG_BIT_OFFSET = 10U;
    static constexpr auto SMALL_FLAG_BIT = (1U << SMALL_FLAG_BIT_OFFSET);

    static constexpr auto DIST_SYM_LEN = 5U;
    static constexpr auto DIST_SYM_MASK = ((1U << DIST_SYM_LEN) - 1U);

    auto next_bits = bitReader.peek<ISAL_DECODE_SHORT_BITS>();

    uint32_t next_sym = m_huffmanCode.short_code_lookup[next_bits];
    uint32_t bit_count{0};
    if (LIKELY((next_sym & SMALL_FLAG_BIT) == 0)) [[likely]] {

      bit_count = next_sym >> SMALL_SHORT_CODE_LEN_OFFSET;
      bitReader.seekAfterPeek(bit_count);
    } else {

      next_bits = bitReader.peek((next_sym - SMALL_FLAG_BIT) >>
                                 SMALL_SHORT_CODE_LEN_OFFSET);
      next_sym =
          m_huffmanCode.long_code_lookup[(next_sym & SMALL_SHORT_SYM_MASK) +
                                         (next_bits >> ISAL_DECODE_SHORT_BITS)];
      bit_count = next_sym >> SMALL_LONG_CODE_LEN_OFFSET;
      bitReader.seekAfterPeek(bit_count);
    }

    if (bit_count == 0) {
      return std::nullopt;
    }
    return next_sym & DIST_SYM_MASK;
  }

private:
  Error m_error{Error::INVALID_HUFFMAN_CODE};
  inflate_huff_code_small m_huffmanCode;
};
} // namespace rapidgzip
