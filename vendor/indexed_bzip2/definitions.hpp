#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <string>

#include <BitReader.hpp>

namespace rapidgzip {
namespace gzip {

using BitReader = BitReader<false, uint64_t>;
}

constexpr auto BYTE_SIZE = 8U;

namespace deflate {
constexpr size_t MAX_WINDOW_SIZE = 32ULL * 1024ULL;

constexpr size_t MAX_UNCOMPRESSED_SIZE = std::numeric_limits<uint16_t>::max();

constexpr uint8_t MAX_CODE_LENGTH = 15;

constexpr uint32_t PRECODE_COUNT_BITS = 4;
constexpr uint32_t MAX_PRECODE_COUNT = 19;
constexpr uint32_t PRECODE_BITS = 3;
constexpr uint32_t MAX_PRECODE_LENGTH = (1U << PRECODE_BITS) - 1U;
static_assert(MAX_PRECODE_LENGTH == 7);
alignas(8) static constexpr std::array<uint8_t,
                                       MAX_PRECODE_COUNT> PRECODE_ALPHABET = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

constexpr size_t MAX_LITERAL_OR_LENGTH_SYMBOLS = 286;

constexpr uint8_t MAX_DISTANCE_SYMBOL_COUNT = 30;

constexpr size_t MAX_LITERAL_HUFFMAN_CODE_COUNT = 512;
constexpr size_t MAX_RUN_LENGTH = 258;

constexpr uint16_t END_OF_BLOCK_SYMBOL = 256;

enum class CompressionType : uint8_t {
  UNCOMPRESSED = 0b00,
  FIXED_HUFFMAN = 0b01,
  DYNAMIC_HUFFMAN = 0b10,
  RESERVED = 0b11,
};

[[nodiscard]] inline std::string
toString(CompressionType compressionType) noexcept {
  switch (compressionType) {
  case CompressionType::UNCOMPRESSED:
    return "Uncompressed";
  case CompressionType::FIXED_HUFFMAN:
    return "Fixed Huffman";
  case CompressionType::DYNAMIC_HUFFMAN:
    return "Dynamic Huffman";
  case CompressionType::RESERVED:
    return "Reserved";
  }
  return "Unknown";
}
} // namespace deflate

enum StoppingPoint : uint32_t {
  NONE = 0U,
  END_OF_STREAM_HEADER = 1U << 0U,
  END_OF_STREAM = 1U << 1U,
  END_OF_BLOCK_HEADER = 1U << 2U,
  END_OF_BLOCK = 1U << 3U,
  ALL = 0xFFFF'FFFFU,
};

[[nodiscard]] inline std::string toString(StoppingPoint stoppingPoint) {
  // *INDENT-OFF*
  switch (stoppingPoint) {
  case StoppingPoint::NONE:
    return "None";
  case StoppingPoint::END_OF_STREAM_HEADER:
    return "End of Stream Header";
  case StoppingPoint::END_OF_STREAM:
    return "End of Stream";
  case StoppingPoint::END_OF_BLOCK_HEADER:
    return "End of Block Header";
  case StoppingPoint::END_OF_BLOCK:
    return "End of Block";
  case StoppingPoint::ALL:
    return "All";
  };
  return "Unknown";
  // *INDENT-ON*
}

struct BlockBoundary {
  size_t encodedOffset{0};
  size_t decodedOffset{0};

  [[nodiscard]] bool operator==(const BlockBoundary &other) const {
    return (encodedOffset == other.encodedOffset) &&
           (decodedOffset == other.decodedOffset);
  }
};
} // namespace rapidgzip
