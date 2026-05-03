#pragma once

#include <algorithm>
#include <cassert>
#include <limits>
#include <utility>

#include <BitManipulation.hpp>
#include <common.hpp>
#include <definitions.hpp>

namespace rapidgzip::blockfinder {

[[nodiscard]] inline std::pair<size_t, size_t>
seekToNonFinalUncompressedDeflateBlock(
    gzip::BitReader &bitReader,
    size_t const untilOffset = std::numeric_limits<size_t>::max()) {
  static constexpr auto DEFLATE_MAGIC_BIT_COUNT = 3U;

  static constexpr uint32_t MAX_PRECEDING_BITS =
      DEFLATE_MAGIC_BIT_COUNT + (BYTE_SIZE - 1U);
  static constexpr uint32_t MAX_PRECEDING_BYTES =
      ceilDiv(MAX_PRECEDING_BITS, BYTE_SIZE) * BYTE_SIZE;

  try {
    auto untilOffsetSizeMember =
        untilOffset >= std::numeric_limits<size_t>::max() - MAX_PRECEDING_BYTES
            ? std::numeric_limits<size_t>::max()
            : untilOffset + MAX_PRECEDING_BYTES;
    auto fileSize = bitReader.size();
    if (fileSize) {
      untilOffsetSizeMember = std::min(untilOffsetSizeMember, *fileSize);
    }

    const auto startOffset = bitReader.tell();

    const auto startOffsetByte = std::max(
        static_cast<size_t>(BYTE_SIZE),
        ceilDiv(startOffset + DEFLATE_MAGIC_BIT_COUNT, BYTE_SIZE) * BYTE_SIZE);
    if (startOffsetByte < untilOffsetSizeMember) {
      bitReader.seekTo(startOffsetByte);
    }

    auto size = bitReader.read<3U * BYTE_SIZE>() << BYTE_SIZE;
    for (size_t offset = startOffsetByte; offset < untilOffsetSizeMember;
         offset += BYTE_SIZE) {

      size =
          (size >> BYTE_SIZE) | (bitReader.read<BYTE_SIZE>() << 3U * BYTE_SIZE);
      if (LIKELY(((size ^ (size >> 16U)) & nLowestBitsSet<uint32_t>(16)) !=
                 0xFFFFU)) [[likely]] {
        continue;
      }

      const auto oldOffset = offset + 4UL * BYTE_SIZE;
      assert(oldOffset == bitReader.tell());

      bitReader.seekTo(offset - MAX_PRECEDING_BITS);
      const auto previousBits = bitReader.peek<MAX_PRECEDING_BITS>();

      static constexpr auto MAGIC_BITS_MASK =
          0b111ULL << (MAX_PRECEDING_BITS - DEFLATE_MAGIC_BIT_COUNT);
      if (LIKELY((previousBits & MAGIC_BITS_MASK) != 0)) [[likely]] {
        bitReader.seekTo(oldOffset);
        continue;
      }

      size_t trailingZeros = DEFLATE_MAGIC_BIT_COUNT;
      for (size_t j = trailingZeros + 1; j <= MAX_PRECEDING_BITS; ++j) {
        if ((previousBits & (1U << (MAX_PRECEDING_BITS - j))) != 0) {
          break;
        }
        trailingZeros = j;
      }

      if ((offset - DEFLATE_MAGIC_BIT_COUNT >= startOffset) &&
          (offset - trailingZeros < untilOffset)) {
        return std::make_pair(offset - trailingZeros,
                              offset - DEFLATE_MAGIC_BIT_COUNT);
      }

      bitReader.seekTo(oldOffset);
    }
  } catch (const gzip::BitReader::EndOfFileReached &) {
  }

  return std::make_pair(std::numeric_limits<size_t>::max(),
                        std::numeric_limits<size_t>::max());
}
} // namespace rapidgzip::blockfinder
