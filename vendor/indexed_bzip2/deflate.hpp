

#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <DecodedDataView.hpp>
#include <Error.hpp>
#include <MarkerReplacement.hpp>

#include <HuffmanCodingReversedBitsCached.hpp>
#include <HuffmanCodingReversedBitsCachedCompressed.hpp>

#ifdef LIBRAPIDARCHIVE_WITH_ISAL

#include <HuffmanCodingISAL.hpp>
#elif defined(WITH_DEFLATE_SPECIFIC_HUFFMAN_DECODER)
#include <HuffmanCodingShortBitsCachedDeflate.hpp>
#elif defined(WITH_MULTI_CACHED_HUFFMAN_DECODER)
#include <HuffmanCodingShortBitsMultiCached.hpp>
#else

#include <HuffmanCodingShortBitsCached.hpp>
#endif

#include "RFCTables.hpp"
#include "definitions.hpp"

namespace rapidgzip::deflate {

#ifdef LIBRAPIDARCHIVE_WITH_ISAL
using LiteralOrLengthHuffmanCoding = HuffmanCodingISAL;
#elif defined(WITH_DEFLATE_SPECIFIC_HUFFMAN_DECODER)
using LiteralOrLengthHuffmanCoding = HuffmanCodingShortBitsCachedDeflate<11>;
#elif defined(WITH_MULTI_CACHED_HUFFMAN_DECODER)
using LiteralOrLengthHuffmanCoding = HuffmanCodingShortBitsMultiCached<11>;
#else

using LiteralOrLengthHuffmanCoding =
    HuffmanCodingShortBitsCached<uint16_t, MAX_CODE_LENGTH, uint16_t,
                                 MAX_LITERAL_HUFFMAN_CODE_COUNT, 11, true,
                                 true>;
#endif

using FixedHuffmanCoding =
    HuffmanCodingReversedBitsCached<uint16_t, MAX_CODE_LENGTH, uint16_t,
                                    MAX_LITERAL_OR_LENGTH_SYMBOLS + 2>;

using PrecodeHuffmanCoding =
    HuffmanCodingReversedBitsCachedCompressed<uint8_t, MAX_PRECODE_LENGTH,
                                              uint8_t, MAX_PRECODE_COUNT>;

using DistanceHuffmanCoding =
    HuffmanCodingReversedBitsCached<uint16_t, MAX_CODE_LENGTH, uint8_t,
                                    MAX_DISTANCE_SYMBOL_COUNT>;

using LiteralAndDistanceCLBuffer =
    std::array<uint8_t,
               MAX_LITERAL_OR_LENGTH_SYMBOLS + MAX_DISTANCE_SYMBOL_COUNT + 256>;

[[nodiscard]] constexpr FixedHuffmanCoding createFixedHC() {
  std::array<uint8_t, MAX_LITERAL_OR_LENGTH_SYMBOLS + 2>
      encodedFixedHuffmanTree{};
  for (size_t i = 0; i < encodedFixedHuffmanTree.size(); ++i) {

    if (i < 144) {
      encodedFixedHuffmanTree[i] = 8;
    } else if (i < 256) {
      encodedFixedHuffmanTree[i] = 9;
    } else if (i < 280) {
      encodedFixedHuffmanTree[i] = 7;
    } else {
      encodedFixedHuffmanTree[i] = 8;
    }
  }

  FixedHuffmanCoding result;
  const auto error = result.initializeFromLengths(
      {encodedFixedHuffmanTree.data(), encodedFixedHuffmanTree.size()});
  if (error != Error::NONE) {
    throw std::logic_error("Fixed Huffman Tree could not be created!");
  }

  return result;
}

namespace {

[[nodiscard]] forceinline Error readDistanceAndLiteralCodeLengths(
    LiteralAndDistanceCLBuffer &literalCL, gzip::BitReader &bitReader,
    const PrecodeHuffmanCoding &precodeCoding, const size_t literalCLSize,
    const std::function<uint8_t(uint8_t)> &translateSymbol =
        [](uint8_t symbol) { return symbol; }) {
  size_t i = 0;
  for (; i < literalCLSize;) {
    const auto decoded = precodeCoding.decode(bitReader);
    if (!decoded) {
      return Error::INVALID_HUFFMAN_CODE;
    }
    const auto code = translateSymbol(*decoded);

    if (code <= 15) {
      literalCL[i] = code;
      ++i;
    } else if (code == 16) {
      if (i == 0) {
        return Error::INVALID_CL_BACKREFERENCE;
      }
      const auto lastValue = literalCL[i - 1];

      literalCL[i + 0] = lastValue;
      literalCL[i + 1] = lastValue;
      literalCL[i + 2] = lastValue;
      literalCL[i + 3] = lastValue;
      literalCL[i + 4] = lastValue;
      literalCL[i + 5] = lastValue;

      i += bitReader.read<2>() + 3;
    } else if (code == 17) {

      literalCL[i + 0] = 0;
      literalCL[i + 1] = 0;
      literalCL[i + 2] = 0;
      literalCL[i + 3] = 0;
      literalCL[i + 4] = 0;
      literalCL[i + 5] = 0;
      literalCL[i + 6] = 0;
      literalCL[i + 7] = 0;
      literalCL[i + 8] = 0;
      literalCL[i + 9] = 0;

      i += bitReader.read<3>() + 3;
    } else if (code == 18) {

#if defined(__GNUC__)
#pragma GCC unroll 16
#endif
      for (size_t j = 0; j < 11U + (1U << 7U) - 1U; ++j) {
        literalCL[i + j] = 0;
      }
      i += bitReader.read<7>() + 11;
    }
  }

  return i == literalCLSize ? Error::NONE : Error::EXCEEDED_LITERAL_RANGE;
}
} // namespace

class BlockStatistics {
public:
  uint64_t failedPrecodeInit{0};
  uint64_t failedDistanceInit{0};
  uint64_t failedLiteralInit{0};
  uint64_t failedPrecodeApply{0};
  uint64_t missingEOBSymbol{0};

  std::array<uint64_t, 16> precodeCLHistogram{};

  struct {
    uint32_t precode{0};
    uint32_t distance{0};
    uint32_t literal{0};
  } codeCounts;

  struct {
    uint64_t literal{0};
    uint64_t backreference{0};
    uint64_t copies{0};
  } symbolTypes;

  struct {
    double readDynamicHeader{0};
    double readPrecode{0};
    double createPrecodeHC{0};
    double applyPrecodeHC{0};
    double createDistanceHC{0};
    double createLiteralHC{0};
    double readData{0};
  } durations;

  struct {
    using TimePoint =
        std::chrono::time_point<std::chrono::high_resolution_clock>;

    TimePoint readDynamicStart;
    TimePoint readPrecode;
    TimePoint createdPrecodeHC;
    TimePoint appliedPrecodeHC;
    TimePoint createdDistanceHC;

    TimePoint readDataStart;
  } times;
};

template <bool ENABLE_STATISTICS = false> class Block : public BlockStatistics {
public:
  using CompressionType = deflate::CompressionType;

  struct Backreference {
    uint16_t distance{0};
    uint16_t length{0};
  };

public:
  [[nodiscard]] bool eob() const noexcept { return m_atEndOfBlock; }

  [[nodiscard]] constexpr bool eos() const noexcept {
    return m_atEndOfBlock && m_isLastBlock;
  }

  [[nodiscard]] constexpr bool eof() const noexcept { return m_atEndOfFile; }

  [[nodiscard]] constexpr bool isLastBlock() const noexcept {
    return m_isLastBlock;
  }

  [[nodiscard]] constexpr CompressionType compressionType() const noexcept {
    return m_compressionType;
  }

  [[nodiscard]] constexpr uint8_t padding() const noexcept { return m_padding; }

  [[nodiscard]] std::pair<DecodedDataView, Error>
  read(gzip::BitReader &bitReader,
       size_t nMaxToDecode = std::numeric_limits<size_t>::max());

  template <bool treatLastBlockAsError = false>
  [[nodiscard]] Error readHeader(gzip::BitReader &bitReader);

  [[nodiscard]] Error readDynamicHuffmanCoding(gzip::BitReader &bitReader);

  [[nodiscard]] constexpr size_t uncompressedSize() const noexcept {
    return m_compressionType == CompressionType::UNCOMPRESSED
               ? m_uncompressedSize
               : 0;
  }

  void setInitialWindow(VectorView<uint8_t> const &initialWindow = {});

  [[nodiscard]] constexpr bool isValid() const noexcept {
    switch (m_compressionType) {
    case CompressionType::RESERVED:
      return false;

    case CompressionType::UNCOMPRESSED:
      return true;

    case CompressionType::FIXED_HUFFMAN:
#ifdef _MSC_VER
      return true;
#else
      return m_fixedHC.isValid();
#endif

    case CompressionType::DYNAMIC_HUFFMAN:
      return m_literalHC.isValid();
    }

    return false;
  }

  [[nodiscard]] constexpr const auto &precodeCL() const noexcept {
    return m_precodeCL;
  }

  [[nodiscard]] constexpr const auto &distanceAndLiteralCL() const noexcept {
    return m_literalCL;
  }

  void setTrackBackreferences(bool enable) noexcept {
    m_trackBackreferences = enable;
  }

  [[nodiscard]] bool trackBackreferences() const noexcept {
    return m_trackBackreferences;
  }

  [[nodiscard]] const std::vector<Backreference> &
  backreferences() const noexcept {
    return m_backreferences;
  }

  void reset(const std::optional<VectorView<uint8_t>> initialWindow = {}) {
    m_uncompressedSize = 0;

    m_atEndOfBlock = false;
    m_atEndOfFile = false;

    m_isLastBlock = false;
    m_compressionType = CompressionType::RESERVED;
    m_padding = 0;

    m_windowPosition = 0;
    m_containsMarkerBytes = true;
    m_decodedBytes = 0;

    m_distanceToLastMarkerByte = 0;

    m_trackBackreferences = false;
    m_decodedBytesAtBlockStart = 0;
    m_backreferences.clear();

    if (initialWindow) {
      setInitialWindow(*initialWindow);
    } else {
      m_window16 = initializeMarkedWindowBuffer();
    }
  }

private:
  template <typename Window>
  forceinline void appendToWindow(Window &window,
                                  typename Window::value_type decodedSymbol);

  template <typename Window>
  forceinline void
  appendToWindowUnsafe(Window &window,
                       typename Window::value_type decodedSymbol);

  template <typename Window>
  forceinline void resolveBackreference(Window &window, uint16_t distance,
                                        uint16_t length, size_t nBytesRead);

  template <typename Window>
  [[nodiscard]] std::pair<size_t, Error>
  readInternal(gzip::BitReader &bitReader, size_t nMaxToDecode, Window &window);

  template <typename Window>
  [[nodiscard]] std::pair<size_t, Error>
  readInternalUncompressed(gzip::BitReader &bitReader, Window &window);

  template <typename Window, typename HuffmanCoding>
  [[nodiscard]] std::pair<size_t, Error>
  readInternalCompressed(gzip::BitReader &bitReader, size_t nMaxToDecode,
                         Window &window, const HuffmanCoding &coding);

#if defined(LIBRAPIDARCHIVE_WITH_ISAL) ||                                      \
    defined(WITH_MULTI_CACHED_HUFFMAN_DECODER)
  template <typename Window>
  [[nodiscard]] std::pair<size_t, Error>
  readInternalCompressedMultiCached(gzip::BitReader &bitReader,
                                    size_t nMaxToDecode, Window &window,
                                    const LiteralOrLengthHuffmanCoding &coding);

#elif defined(WITH_DEFLATE_SPECIFIC_HUFFMAN_DECODER)

  template <typename Window>
  [[nodiscard]] std::pair<size_t, Error>
  readInternalCompressedSpecialized(gzip::BitReader &bitReader,
                                    size_t nMaxToDecode, Window &window,
                                    const LiteralOrLengthHuffmanCoding &coding);

#endif

  [[nodiscard]] forceinline std::pair<uint16_t, Error>
  getDistance(gzip::BitReader &bitReader) const;

  template <typename Window, typename Symbol = typename Window::value_type,
            typename View = VectorView<Symbol>>
  [[nodiscard]] static std::array<View, 2>
  lastBuffers(const Window &window, size_t position, size_t size) {
    if (size > window.size()) {
      throw std::invalid_argument(
          "Requested more bytes than fit in the buffer. Data is missing!");
    }

    std::array<View, 2> result{};
    if (size == 0) {
      return result;
    }

    const auto begin =
        (position + window.size() - (size % window.size())) % window.size();
    if (begin < position) {
      result[0] = View(window.data() + begin, position - begin);
      return result;
    }

    result[0] = View(window.data() + begin, window.size() - begin);
    result[1] = View(window.data(), position);
    return result;
  }

private:
  using PreDecodedBuffer = std::array<uint16_t, 2 * MAX_WINDOW_SIZE>;
  using DecodedBuffer =
      WeakArray<std::uint8_t,
                PreDecodedBuffer().size() * sizeof(uint16_t) / sizeof(uint8_t)>;

  static_assert(
      PreDecodedBuffer().size() * sizeof(uint16_t) / sizeof(uint8_t) >=
          MAX_UNCOMPRESSED_SIZE,
      "Buffer should at least be able to fit one uncompressed block.");
  static_assert(std::min(PreDecodedBuffer().size(), PreDecodedBuffer().size() *
                                                        sizeof(uint16_t) /
                                                        sizeof(uint8_t)) >=
                    MAX_WINDOW_SIZE + MAX_RUN_LENGTH,
                "Buffers should at least be able to fit the back-reference "
                "window plus the maximum match length.");

private:
  [[nodiscard]] static const PreDecodedBuffer &initializeMarkedWindowBuffer() {
    static const PreDecodedBuffer markers = []() {
      PreDecodedBuffer result{};
      for (size_t i = 0; i < MAX_WINDOW_SIZE; ++i) {
        result[result.size() - MAX_WINDOW_SIZE + i] = i + MAX_WINDOW_SIZE;
      }
      return result;
    }();
    return markers;
  }

  [[nodiscard]] constexpr DecodedBuffer getWindow() noexcept {
    return DecodedBuffer{reinterpret_cast<std::uint8_t *>(m_window16.data())};
  }

private:
  uint16_t m_uncompressedSize{0};

private:
  mutable bool m_atEndOfBlock{false};
  mutable bool m_atEndOfFile{false};

  bool m_isLastBlock{false};
  CompressionType m_compressionType{CompressionType::RESERVED};

  uint8_t m_padding{0};

#ifndef _MSC_VER

  static constexpr FixedHuffmanCoding m_fixedHC = createFixedHC();
#endif
  LiteralOrLengthHuffmanCoding m_literalHC;

  DistanceHuffmanCoding m_distanceHC;

  alignas(64) PreDecodedBuffer m_window16{initializeMarkedWindowBuffer()};

public:
  size_t m_windowPosition{0};

  bool m_containsMarkerBytes{true};

  size_t m_decodedBytes{0};

  size_t m_distanceToLastMarkerByte{0};

  bool m_trackBackreferences{false};
  size_t m_decodedBytesAtBlockStart{0};
  std::vector<Backreference> m_backreferences;

  alignas(64) std::array<uint8_t, MAX_PRECODE_COUNT> m_precodeCL{};
  alignas(64) PrecodeHuffmanCoding m_precodeHC{};
  alignas(64) LiteralAndDistanceCLBuffer m_literalCL{};
};

template <bool ENABLE_STATISTICS>
template <bool treatLastBlockAsError>
Error Block<ENABLE_STATISTICS>::readHeader(gzip::BitReader &bitReader) {
  try {
    m_isLastBlock = bitReader.read<1>();
  } catch (const gzip::BitReader::EndOfFileReached &) {
    return Error::END_OF_FILE;
  }

  if constexpr (treatLastBlockAsError) {
    if (m_isLastBlock) {
      return Error::UNEXPECTED_LAST_BLOCK;
    }
  }
  m_compressionType = static_cast<CompressionType>(bitReader.read<2>());

  Error error = Error::NONE;

  switch (m_compressionType) {
  case CompressionType::UNCOMPRESSED: {

    if (bitReader.tell() % BYTE_SIZE != 0) {
      m_padding = static_cast<uint8_t>(
          bitReader.read(BYTE_SIZE - (bitReader.tell() % BYTE_SIZE)));
      if (m_padding != 0) {
        return Error::NON_ZERO_PADDING;
      }
    }

    m_uncompressedSize = bitReader.read<2 * BYTE_SIZE>();
    const auto negatedLength = bitReader.read<2 * BYTE_SIZE>();
    if (m_uncompressedSize != static_cast<uint16_t>(~negatedLength)) {
      return Error::LENGTH_CHECKSUM_MISMATCH;
    }
    break;
  }

  case CompressionType::FIXED_HUFFMAN:
    break;

  case CompressionType::DYNAMIC_HUFFMAN:
    error = readDynamicHuffmanCoding(bitReader);
    break;

  case CompressionType::RESERVED:
    return Error::INVALID_COMPRESSION;
  };

  m_atEndOfBlock = false;
  m_decodedBytesAtBlockStart = m_decodedBytes;
  m_backreferences.clear();

  return error;
}

template <bool ENABLE_STATISTICS>
Error Block<ENABLE_STATISTICS>::readDynamicHuffmanCoding(
    gzip::BitReader &bitReader) {
  if constexpr (ENABLE_STATISTICS) {
    times.readDynamicStart = now();
  }

  const auto literalCodeCount = 257 + bitReader.read<5>();
  if (literalCodeCount > MAX_LITERAL_OR_LENGTH_SYMBOLS) {
    durations.readDynamicHeader += duration(times.readDynamicStart);
    return Error::EXCEEDED_LITERAL_RANGE;
  }
  const auto distanceCodeCount = 1 + bitReader.read<5>();
  if (distanceCodeCount > MAX_DISTANCE_SYMBOL_COUNT) {
    durations.readDynamicHeader += duration(times.readDynamicStart);
    return Error::EXCEEDED_DISTANCE_RANGE;
  }
  const auto codeLengthCount = 4 + bitReader.read<4>();

  if constexpr (ENABLE_STATISTICS) {
    this->precodeCLHistogram[codeLengthCount - 4]++;
    this->codeCounts.precode = codeLengthCount;
    this->codeCounts.distance = distanceCodeCount;
    this->codeCounts.literal = literalCodeCount;
  }

  std::memset(m_precodeCL.data(), 0,
              m_precodeCL.size() * sizeof(m_precodeCL[0]));
  for (size_t i = 0; i < codeLengthCount; ++i) {
    m_precodeCL[PRECODE_ALPHABET[i]] = bitReader.read<PRECODE_BITS>();
  }

  if constexpr (ENABLE_STATISTICS) {
    times.readPrecode = now();
    durations.readPrecode +=
        duration(times.readDynamicStart, times.readPrecode);
  }

  auto error = m_precodeHC.initializeFromLengths(
      VectorView<uint8_t>(m_precodeCL.data(), m_precodeCL.size()));

  if constexpr (ENABLE_STATISTICS) {
    times.createdPrecodeHC = now();
    this->durations.createPrecodeHC +=
        duration(times.readPrecode, times.createdPrecodeHC);
  }

  if (error != Error::NONE) {
    if constexpr (ENABLE_STATISTICS) {
      this->failedPrecodeInit++;
      durations.readDynamicHeader += duration(times.readDynamicStart);
    }
    return error;
  }

  auto precodeApplyError =
      readDistanceAndLiteralCodeLengths(m_literalCL, bitReader, m_precodeHC,
                                        literalCodeCount + distanceCodeCount);

  if constexpr (ENABLE_STATISTICS) {
    times.appliedPrecodeHC = now();
    durations.applyPrecodeHC +=
        duration(times.createdPrecodeHC, times.appliedPrecodeHC);
  }

  if (precodeApplyError != Error::NONE) {
    if constexpr (ENABLE_STATISTICS) {
      this->failedPrecodeApply++;
      durations.readDynamicHeader += duration(times.readDynamicStart);
    }
    return precodeApplyError;
  }

  if (m_literalCL[deflate::END_OF_BLOCK_SYMBOL] == 0) {
    if constexpr (ENABLE_STATISTICS) {
      durations.readDynamicHeader += duration(times.readDynamicStart);
      this->missingEOBSymbol++;
    }
    return Error::INVALID_CODE_LENGTHS;
  }

  error = m_distanceHC.initializeFromLengths(VectorView<uint8_t>(
      m_literalCL.data() + literalCodeCount, distanceCodeCount));

  if constexpr (ENABLE_STATISTICS) {
    times.createdDistanceHC = now();
    durations.createDistanceHC +=
        duration(times.appliedPrecodeHC, times.createdDistanceHC);
  }

  if (error != Error::NONE) {
    if constexpr (ENABLE_STATISTICS) {
      durations.readDynamicHeader += duration(times.readDynamicStart);
      this->failedDistanceInit++;
    }
    return error;
  }

#ifdef WITH_DEFLATE_SPECIFIC_HUFFMAN_DECODER
  error = m_literalHC.initializeFromLengths(
      VectorView<uint8_t>(m_literalCL.data(), literalCodeCount), m_distanceHC);
#else
  error = m_literalHC.initializeFromLengths(
      VectorView<uint8_t>(m_literalCL.data(), literalCodeCount));
#endif
  if (error != Error::NONE) {
    if constexpr (ENABLE_STATISTICS) {
      this->failedLiteralInit++;
    }
  }

  if constexpr (ENABLE_STATISTICS) {
    const auto tFinish = now();
    durations.createLiteralHC += duration(times.createdDistanceHC, tFinish);
    durations.readDynamicHeader += duration(times.readDynamicStart, tFinish);
  }

  return error;
}

template <bool ENABLE_STATISTICS>
forceinline std::pair<uint16_t, Error>
Block<ENABLE_STATISTICS>::getDistance(gzip::BitReader &bitReader) const {
  uint16_t distance = 0;
  if (m_compressionType == CompressionType::FIXED_HUFFMAN) {
    distance = reverseBits(static_cast<uint8_t>(bitReader.read<5>())) >> 3U;
    if (UNLIKELY(distance >= MAX_DISTANCE_SYMBOL_COUNT)) [[unlikely]] {
      return {0, Error::EXCEEDED_DISTANCE_RANGE};
    }
  } else {
    const auto decodedDistance = m_distanceHC.decode(bitReader);
    if (UNLIKELY(!decodedDistance)) [[unlikely]] {
      return {0, Error::INVALID_HUFFMAN_CODE};
    }
    distance = static_cast<uint16_t>(*decodedDistance);
  }

  if (distance <= 3U) {
    distance += 1U;
  } else if (distance <= 29U) {
    const auto extraBitsCount = (distance - 2U) / 2U;
    const auto extraBits = bitReader.read(extraBitsCount);
    distance = distanceLUT[distance] + extraBits;
  } else {
    throw std::logic_error("Invalid distance codes encountered!");
  }

  return {distance, Error::NONE};
}

template <bool ENABLE_STATISTICS>
std::pair<DecodedDataView, Error>
Block<ENABLE_STATISTICS>::read(gzip::BitReader &bitReader,
                               size_t nMaxToDecode) {
  if (eob()) {
    return {{}, Error::NONE};
  }

  if (m_compressionType == CompressionType::RESERVED) {
    throw std::domain_error("Invalid deflate compression type!");
  }

  if constexpr (ENABLE_STATISTICS) {
    times.readDataStart = now();
  }

  DecodedDataView result;
  const auto window = getWindow();

  if (m_compressionType == CompressionType::UNCOMPRESSED) {
    std::optional<size_t> nBytesRead;
    if (m_uncompressedSize >= MAX_WINDOW_SIZE) {

      m_windowPosition = m_uncompressedSize;
      nBytesRead = bitReader.read(reinterpret_cast<char *>(window.data()),
                                  m_uncompressedSize);
    } else if (m_containsMarkerBytes &&
               (m_distanceToLastMarkerByte + m_uncompressedSize >=
                MAX_WINDOW_SIZE)) {

      assert(m_distanceToLastMarkerByte <= m_decodedBytes);

      std::vector<uint8_t> remainingData(MAX_WINDOW_SIZE - m_uncompressedSize);
      size_t downcastedSize{0};
      for (const auto buffer :
           lastBuffers(m_window16, m_windowPosition, remainingData.size())) {
        if (std::any_of(buffer.begin(), buffer.end(),
                        [](const auto symbol) { return symbol > 255; })) {
          throw std::logic_error(
              "Encountered marker byte even though there shouldn't be one!");
        }

        std::transform(
            buffer.begin(), buffer.end(), remainingData.data() + downcastedSize,
            [](const auto symbol) { return static_cast<uint8_t>(symbol); });
        downcastedSize += buffer.size();
      }

      m_windowPosition = MAX_WINDOW_SIZE;

      std::memcpy(window.data(), remainingData.data(), remainingData.size());
      nBytesRead = bitReader.read(
          reinterpret_cast<char *>(window.data() + remainingData.size()),
          m_uncompressedSize);
    } else if (!m_containsMarkerBytes) {

      m_windowPosition =
          (m_windowPosition + m_uncompressedSize) % window.size();
      size_t totalBytesRead{0};
      auto buffers = lastBuffers<DecodedBuffer, uint8_t, WeakVector<uint8_t>>(
          window, m_windowPosition, m_uncompressedSize);
      for (auto &buffer : buffers) {
        totalBytesRead += bitReader.read(
            reinterpret_cast<char *>(buffer.data()), buffer.size());
      }
      nBytesRead = totalBytesRead;
    }

    if (nBytesRead) {
      m_containsMarkerBytes = false;
      m_atEndOfBlock = true;
      m_decodedBytes += *nBytesRead;

      result.data = lastBuffers(window, m_windowPosition, *nBytesRead);

      if constexpr (ENABLE_STATISTICS) {
        durations.readData += duration(times.readDataStart);
      }

      return {result, *nBytesRead == m_uncompressedSize
                          ? Error::NONE
                          : Error::EOF_UNCOMPRESSED};
    }
  }

  size_t nBytesRead{0};
  auto error = Error::NONE;
  if (m_containsMarkerBytes) {

    std::tie(nBytesRead, error) =
        readInternal(bitReader, nMaxToDecode, m_window16);

    if ((m_distanceToLastMarkerByte >= m_window16.size()) ||
        ((m_distanceToLastMarkerByte >= MAX_WINDOW_SIZE) &&
         (m_distanceToLastMarkerByte == m_decodedBytes))) {
      setInitialWindow();
      result.data = lastBuffers(window, m_windowPosition, nBytesRead);
    } else {
      result.dataWithMarkers =
          lastBuffers(m_window16, m_windowPosition, nBytesRead);
    }
  } else {
    std::tie(nBytesRead, error) = readInternal(bitReader, nMaxToDecode, window);
    result.data = lastBuffers(window, m_windowPosition, nBytesRead);
  }

  if constexpr (ENABLE_STATISTICS) {
    durations.readData += duration(times.readDataStart);
  }

  return {result, error};
}

template <bool ENABLE_STATISTICS>
template <typename Window>
inline void Block<ENABLE_STATISTICS>::appendToWindow(
    Window &window, typename Window::value_type decodedSymbol) {
  constexpr bool containsMarkerBytes =
      std::is_same_v<std::decay_t<typename Window::value_type>, uint16_t>;

  if constexpr (containsMarkerBytes) {
    if (decodedSymbol > std::numeric_limits<uint8_t>::max()) {
      m_distanceToLastMarkerByte = 0;
    } else {
      ++m_distanceToLastMarkerByte;
    }
  }

  window[m_windowPosition] = decodedSymbol;
  m_windowPosition++;
  m_windowPosition %= window.size();
}

template <bool ENABLE_STATISTICS>
template <typename Window>
inline void Block<ENABLE_STATISTICS>::appendToWindowUnsafe(
    Window &window, typename Window::value_type decodedSymbol) {
  constexpr bool containsMarkerBytes =
      std::is_same_v<std::decay_t<typename Window::value_type>, uint16_t>;

  if constexpr (containsMarkerBytes) {
    if (decodedSymbol > std::numeric_limits<uint8_t>::max()) {
      m_distanceToLastMarkerByte = 0;
    } else {
      ++m_distanceToLastMarkerByte;
    }
  }

  window[m_windowPosition] = decodedSymbol;
  m_windowPosition++;
}

template <bool ENABLE_STATISTICS>
template <typename Window>
inline void Block<ENABLE_STATISTICS>::resolveBackreference(
    Window &window, const uint16_t distance, const uint16_t length,
    const size_t nBytesRead) {
  if (m_trackBackreferences) {
    if (m_decodedBytes < m_decodedBytesAtBlockStart) {
      throw std::logic_error(
          "Somehow the decoded bytes counter seems to have shrunk!");
    }
    const auto decodedBytesInBlock =
        m_decodedBytes - m_decodedBytesAtBlockStart + nBytesRead;
    if (distance > decodedBytesInBlock) {
      m_backreferences.emplace_back(
          Backreference{static_cast<uint16_t>(distance - decodedBytesInBlock),
                        std::min(length, distance)});
    }
  }

  constexpr bool containsMarkerBytes =
      std::is_same_v<std::decay_t<decltype(*window.data())>, uint16_t>;

  const auto offset =
      (m_windowPosition + window.size() - distance) % window.size();
  const auto nToCopyPerRepeat = std::min(distance, length);
  assert(nToCopyPerRepeat != 0);

  if (LIKELY(m_windowPosition + length < window.size())) [[likely]] {
    if (LIKELY((length <= distance) && (distance <= m_windowPosition)))
        [[likely]] {
      std::memcpy(&window[m_windowPosition], &window[offset],
                  length * sizeof(window.front()));
      m_windowPosition += length;

      if constexpr (containsMarkerBytes) {
        size_t distanceToLastMarkerByte{0};
        for (; distanceToLastMarkerByte < length; ++distanceToLastMarkerByte) {
          if (window[m_windowPosition - 1 - distanceToLastMarkerByte] >
              std::numeric_limits<uint8_t>::max()) {
            m_distanceToLastMarkerByte = distanceToLastMarkerByte;
            return;
          }
        }
        m_distanceToLastMarkerByte += length;
      }
      return;
    }

    if constexpr (!containsMarkerBytes) {
      if (UNLIKELY(nToCopyPerRepeat == 1)) [[unlikely]] {
        std::memset(&window[m_windowPosition], window[offset], length);
        m_windowPosition += length;
        return;
      }
    }

    for (size_t nCopied = 0; nCopied < length;) {
      for (size_t position = offset;
           (position < offset + nToCopyPerRepeat) && (nCopied < length);
           ++position, ++nCopied) {
        const auto copiedSymbol = window[position % window.size()];
        appendToWindowUnsafe(window, copiedSymbol);
      }
    }
    return;
  }

  for (size_t nCopied = 0; nCopied < length;) {
    for (size_t position = offset;
         (position < offset + nToCopyPerRepeat) && (nCopied < length);
         ++position, ++nCopied) {
      const auto copiedSymbol = window[position % window.size()];
      appendToWindow(window, copiedSymbol);
    }
  }
}

template <bool ENABLE_STATISTICS>
template <typename Window>
std::pair<size_t, Error>
Block<ENABLE_STATISTICS>::readInternal(gzip::BitReader &bitReader,
                                       size_t nMaxToDecode, Window &window) {
  if (m_compressionType == CompressionType::UNCOMPRESSED) {

    return readInternalUncompressed(bitReader, window);
  }

  if (m_compressionType == CompressionType::FIXED_HUFFMAN) {
#ifdef _MSC_VER

    static const auto fixedHC = createFixedHC();
    return readInternalCompressed(bitReader, nMaxToDecode, window, fixedHC);
#else
    return readInternalCompressed(bitReader, nMaxToDecode, window, m_fixedHC);
#endif
  }

#ifdef LIBRAPIDARCHIVE_WITH_ISAL
  if constexpr (std::is_same_v<LiteralOrLengthHuffmanCoding,
                               HuffmanCodingISAL>) {
    return readInternalCompressedMultiCached(bitReader, nMaxToDecode, window,
                                             m_literalHC);
  } else {
    return readInternalCompressed(bitReader, nMaxToDecode, window, m_literalHC);
  }
#elif defined(WITH_MULTI_CACHED_HUFFMAN_DECODER)
  return readInternalCompressedMultiCached(bitReader, nMaxToDecode, window,
                                           m_literalHC);
#elif defined(WITH_DEFLATE_SPECIFIC_HUFFMAN_DECODER)
  return readInternalCompressedSpecialized(bitReader, nMaxToDecode, window,
                                           m_literalHC);
#else
  return readInternalCompressed(bitReader, nMaxToDecode, window, m_literalHC);
#endif
}

template <bool ENABLE_STATISTICS>
template <typename Window>
std::pair<size_t, Error>
Block<ENABLE_STATISTICS>::readInternalUncompressed(gzip::BitReader &bitReader,
                                                   Window &window) {

  uint32_t totalBytesRead{0};
  std::array<uint8_t, 64> buffer{};
  for (; totalBytesRead + buffer.size() <= m_uncompressedSize;
       totalBytesRead += buffer.size()) {
    const auto nBytesRead =
        bitReader.read(reinterpret_cast<char *>(buffer.data()), buffer.size());
    for (size_t i = 0; i < nBytesRead; ++i) {
      appendToWindow(window, buffer[i]);
    }
  }
  for (; totalBytesRead < m_uncompressedSize; ++totalBytesRead) {
    appendToWindow(window, static_cast<uint8_t>(bitReader.read<BYTE_SIZE>()));
  }
  m_atEndOfBlock = true;
  m_decodedBytes += m_uncompressedSize;
  return {m_uncompressedSize, Error::NONE};
}

template <bool ENABLE_STATISTICS>
template <typename Window, typename HuffmanCoding>
std::pair<size_t, Error> Block<ENABLE_STATISTICS>::readInternalCompressed(
    gzip::BitReader &bitReader, size_t nMaxToDecode, Window &window,
    const HuffmanCoding &coding) {
  if (!coding.isValid()) {
    throw std::invalid_argument(
        "No Huffman coding loaded! Call readHeader first!");
  }

  constexpr bool containsMarkerBytes =
      std::is_same_v<std::decay_t<decltype(*window.data())>, uint16_t>;

  nMaxToDecode = std::min(nMaxToDecode, window.size() - MAX_RUN_LENGTH);

  size_t nBytesRead{0};
  for (nBytesRead = 0; nBytesRead < nMaxToDecode;) {
    const auto decoded = coding.decode(bitReader);
    if (!decoded) {
      return {nBytesRead, Error::INVALID_HUFFMAN_CODE};
    }
    auto code = *decoded;

    if (code <= 255) {
      if constexpr (ENABLE_STATISTICS) {
        symbolTypes.literal++;
      }

      appendToWindow(window, code);
      ++nBytesRead;
      continue;
    }

    if (UNLIKELY(code == END_OF_BLOCK_SYMBOL)) [[unlikely]] {
      m_atEndOfBlock = true;
      break;
    }

    if (UNLIKELY(code > 285)) [[unlikely]] {
      return {nBytesRead, Error::INVALID_HUFFMAN_CODE};
    }

    if constexpr (ENABLE_STATISTICS) {
      symbolTypes.backreference++;
    }

    const auto length = getLength(code, bitReader);
    if (length != 0) {
      if constexpr (ENABLE_STATISTICS) {
        symbolTypes.copies += length;
      }
      const auto [distance, error] = getDistance(bitReader);
      if (error != Error::NONE) {
        return {nBytesRead, error};
      }

      if constexpr (!containsMarkerBytes) {
        if (distance > m_decodedBytes + nBytesRead) {
          return {nBytesRead, Error::EXCEEDED_WINDOW_RANGE};
        }
      }

      resolveBackreference(window, distance, length, nBytesRead);
      nBytesRead += length;
    }
  }

  m_decodedBytes += nBytesRead;
  return {nBytesRead, Error::NONE};
}

#if defined(LIBRAPIDARCHIVE_WITH_ISAL) ||                                      \
    defined(WITH_MULTI_CACHED_HUFFMAN_DECODER)
template <bool ENABLE_STATISTICS>
template <typename Window>
std::pair<size_t, Error>
Block<ENABLE_STATISTICS>::readInternalCompressedMultiCached(
    gzip::BitReader &bitReader, size_t nMaxToDecode, Window &window,
    const LiteralOrLengthHuffmanCoding &coding) {
  if (!coding.isValid()) {
    throw std::invalid_argument(
        "No Huffman coding loaded! Call readHeader first!");
  }

  constexpr bool containsMarkerBytes =
      std::is_same_v<std::decay_t<decltype(*window.data())>, uint16_t>;

  nMaxToDecode = std::min(nMaxToDecode, window.size() - MAX_RUN_LENGTH);

  size_t nBytesRead{0};
  for (nBytesRead = 0; nBytesRead < nMaxToDecode;) {
    auto [symbol, symbolCount] = coding.decode(bitReader);
    if (symbolCount == 0) {
      return {nBytesRead, Error::INVALID_HUFFMAN_CODE};
    }

    for (; symbolCount > 0; symbolCount--, symbol >>= 8U) {
      const auto code = static_cast<uint16_t>(symbol & 0xFFFFU);

      if ((code <= 255) || (symbolCount > 1)) {
        if constexpr (ENABLE_STATISTICS) {
          symbolTypes.literal++;
        }

        appendToWindow(window, static_cast<uint8_t>(code));
        ++nBytesRead;
        continue;
      }

      if (UNLIKELY(code == END_OF_BLOCK_SYMBOL)) [[unlikely]] {
        m_atEndOfBlock = true;
        m_decodedBytes += nBytesRead;
        return {nBytesRead, Error::NONE};
      }

      static constexpr auto MAX_LIT_LEN_SYM = 512U;
      if (UNLIKELY(code > MAX_LIT_LEN_SYM)) [[unlikely]] {
        return {nBytesRead, Error::INVALID_HUFFMAN_CODE};
      }

      if constexpr (ENABLE_STATISTICS) {
        symbolTypes.backreference++;
      }

      const auto length = symbol - 254U;
      if (length != 0) {
        if constexpr (ENABLE_STATISTICS) {
          symbolTypes.copies += length;
        }
        const auto [distance, error] = getDistance(bitReader);
        if (error != Error::NONE) {
          return {nBytesRead, error};
        }

        if constexpr (!containsMarkerBytes) {
          if (distance > m_decodedBytes + nBytesRead) {
            return {nBytesRead, Error::EXCEEDED_WINDOW_RANGE};
          }
        }

        resolveBackreference(window, distance, length, nBytesRead);
        nBytesRead += length;
      }
    }
  }

  m_decodedBytes += nBytesRead;
  return {nBytesRead, Error::NONE};
}

#elif defined(WITH_DEFLATE_SPECIFIC_HUFFMAN_DECODER)

template <bool ENABLE_STATISTICS>
template <typename Window>
std::pair<size_t, Error>
Block<ENABLE_STATISTICS>::readInternalCompressedSpecialized(
    gzip::BitReader &bitReader, size_t nMaxToDecode, Window &window,
    const LiteralOrLengthHuffmanCoding &coding) {
  if (!coding.isValid()) {
    throw std::invalid_argument(
        "No Huffman coding loaded! Call readHeader first!");
  }

  constexpr bool containsMarkerBytes =
      std::is_same_v<std::decay_t<decltype(*window.data())>, uint16_t>;

  nMaxToDecode = std::min(nMaxToDecode, window.size() - MAX_RUN_LENGTH);

  size_t nBytesRead{0};
  LiteralOrLengthHuffmanCoding::CacheEntry cacheEntry;
  for (nBytesRead = 0; nBytesRead < nMaxToDecode;) {
    try {
      cacheEntry = coding.decode(bitReader, m_distanceHC);
    } catch (const Error &errorCode) {
      return {nBytesRead, errorCode};
    }

    switch (cacheEntry.distance) {
    case 0xFFFFU:
      m_atEndOfBlock = true;
      m_decodedBytes += nBytesRead;
      return {nBytesRead, Error::NONE};

    case 0U:
      if constexpr (ENABLE_STATISTICS) {
        symbolTypes.literal++;
      }
      appendToWindow(window, cacheEntry.symbolOrLength);
      ++nBytesRead;
      break;

    default: {
      const auto length = cacheEntry.symbolOrLength + 3U;
      if constexpr (ENABLE_STATISTICS) {
        symbolTypes.backreference++;
        symbolTypes.copies += length;
      }

      if constexpr (!containsMarkerBytes) {
        if (cacheEntry.distance > m_decodedBytes + nBytesRead) {
          return {nBytesRead, Error::EXCEEDED_WINDOW_RANGE};
        }
      }

      resolveBackreference(window, cacheEntry.distance, length, nBytesRead);
      nBytesRead += length;
      break;
    }
    }
  }

  m_decodedBytes += nBytesRead;
  return {nBytesRead, Error::NONE};
}
#endif

template <bool ENABLE_STATISTICS>
void Block<ENABLE_STATISTICS>::setInitialWindow(
    VectorView<uint8_t> const &initialWindow) {
  if (!m_containsMarkerBytes) {
    return;
  }

  const auto window = getWindow();

  if ((m_decodedBytes == 0) && (m_windowPosition == 0)) {
    if (!initialWindow.empty()) {
      std::memcpy(window.data(), initialWindow.data(), initialWindow.size());
      m_windowPosition = initialWindow.size();
      m_decodedBytes = initialWindow.size();
    }
    m_containsMarkerBytes = false;
    return;
  }

  for (size_t i = 0; m_decodedBytes + i < m_window16.size(); ++i) {
    m_window16[(m_windowPosition + i) % m_window16.size()] = 0;
  }
  replaceMarkerBytes({m_window16.data(), m_window16.size()}, initialWindow);

  std::array<uint8_t, decltype(m_window16)().size()> conflatedBuffer{};

  for (size_t i = 0; i < m_window16.size(); ++i) {
    conflatedBuffer[i] = m_window16[(i + m_windowPosition) % m_window16.size()];
  }

  std::memcpy(window.data() + (window.size() - conflatedBuffer.size()),
              conflatedBuffer.data(), conflatedBuffer.size());

  m_windowPosition = 0;

  m_containsMarkerBytes = false;
}

[[nodiscard]] inline bool
verifySparseWindow(gzip::BitReader &bitReader,
                   const std::vector<bool> &windowByteIsRequired,
                   const VectorView<uint8_t> expectedOutput) {
  Block<false> block;

  std::vector<uint8_t> initialWindow(MAX_WINDOW_SIZE, 0);
  for (size_t i = 0; i < windowByteIsRequired.size(); ++i) {
    if (!windowByteIsRequired[i]) {
      initialWindow[i] = 1;
    }
  }
  block.setInitialWindow({initialWindow.data(), initialWindow.size()});

  for (size_t nBytesRead = 0; nBytesRead < MAX_WINDOW_SIZE;) {
    const auto headerError = block.readHeader(bitReader);
    if (headerError == Error::END_OF_FILE) {
      break;
    }
    if (headerError != Error::NONE) {
      throw std::invalid_argument(
          "Failed to decode the deflate block header! " +
          toString(headerError));
    }

    size_t nBytesReadFromBlock{0};
    while ((nBytesRead + nBytesReadFromBlock < MAX_WINDOW_SIZE) &&
           !block.eob()) {
      const auto [view, readError] =
          block.read(bitReader, MAX_WINDOW_SIZE - nBytesRead);
      if (readError != Error::NONE) {
        throw std::invalid_argument("Failed to read deflate block data! " +
                                    toString(readError));
      }

      if (view.dataWithMarkersSize() > 0) {
        throw std::logic_error(
            "Result should not contain markers because we have set a window!");
      }
      for (const auto &buffer : view.data) {
        const auto sizeToCompare =
            std::min(expectedOutput.size() - (nBytesRead + nBytesReadFromBlock),
                     buffer.size());
        if (!std::equal(buffer.data(), buffer.data() + sizeToCompare,
                        expectedOutput.data() + nBytesRead +
                            nBytesReadFromBlock)) {
          return false;
        }
        nBytesReadFromBlock += buffer.size();
      }
    }

    nBytesRead += nBytesReadFromBlock;
    if (block.eos()) {
      break;
    }
  }

  return true;
}

[[nodiscard]] inline std::vector<bool>
getUsedWindowSymbols(gzip::BitReader &bitReader) {
  std::vector<bool> window(MAX_WINDOW_SIZE, false);

  static constexpr bool CHECK_CORRECTNESS{true};

  [[maybe_unused]] std::vector<uint8_t> decompressed;
  [[maybe_unused]] const auto oldOffset = bitReader.tell();
  size_t nBytesRead{0};

  {

    Block<false> block;
    block.setTrackBackreferences(true);

    if constexpr (CHECK_CORRECTNESS) {
      decompressed.assign(MAX_WINDOW_SIZE, 0);
      block.setInitialWindow({decompressed.data(), decompressed.size()});
    }

    for (; nBytesRead < MAX_WINDOW_SIZE;) {

      const auto headerError = block.readHeader(bitReader);
      if (headerError == Error::END_OF_FILE) {
        break;
      }
      if (headerError != Error::NONE) {
        throw std::invalid_argument(
            "Failed to decode the deflate block header! " +
            toString(headerError));
      }

      size_t nBytesReadFromBlock{0};
      while ((nBytesRead + nBytesReadFromBlock < MAX_WINDOW_SIZE) &&
             !block.eob()) {
        const auto [view, readError] =
            block.read(bitReader, MAX_WINDOW_SIZE - nBytesRead);
        if (readError != Error::NONE) {
          throw std::invalid_argument("Failed to read deflate block data! " +
                                      toString(readError));
        }

        if constexpr (CHECK_CORRECTNESS) {
          if (view.dataWithMarkersSize() > 0) {
            throw std::logic_error("Result should not contain markers because "
                                   "we have set a window!");
          }
          for (const auto &buffer : view.data) {
            const auto sizeToCopy = std::min(
                decompressed.size() - (nBytesRead + nBytesReadFromBlock),
                buffer.size());
            if (sizeToCopy == 0) {
              continue;
            }
            std::memcpy(decompressed.data() + nBytesRead + nBytesReadFromBlock,
                        buffer.data(), sizeToCopy);
            nBytesReadFromBlock += sizeToCopy;
          }
        } else {
          nBytesReadFromBlock += view.size();
        }
      }

      const auto &backreferences = block.backreferences();
      for (const auto &reference : backreferences) {

        if (reference.distance < nBytesRead) {
          continue;
        }

        const auto distanceFromEnd = reference.distance - nBytesRead;
        if (distanceFromEnd > window.size()) {
          std::stringstream message;
          message << "The back-reference distance should not exceed "
                     "MAX_WINDOW_SIZE ("
                  << formatBytes(MAX_WINDOW_SIZE)
                  << ") but got: " << formatBytes(distanceFromEnd) << "!";
          throw std::logic_error(std::move(message).str());
        }
        if (reference.length == 0) {
          continue;
        }
        const auto startOffset = window.size() - distanceFromEnd;

        for (size_t i = 0;
             (i < reference.length) && (startOffset + i < window.size()); ++i) {
          window[startOffset + i] = true;
        }
      }

      nBytesRead += nBytesReadFromBlock;
      if (block.eos()) {
        break;
      }
    }
  }

  if constexpr (CHECK_CORRECTNESS) {
    bitReader.seekTo(oldOffset);
    if (!verifySparseWindow(bitReader, window,
                            {decompressed.data(), nBytesRead})) {
      std::stringstream message;
      message << "[Warning] Sparse window detection failed at offset "
              << formatBits(oldOffset) << ". Will fall back to full window\n";
#ifdef RAPIDGZIP_FATAL_PERFORMANCE_WARNINGS
      throw std::logic_error(std::move(message).str());
#else
      std::cerr << std::move(message).str();
#endif
      window.assign(MAX_WINDOW_SIZE, true);
      return window;
    }
  }

  return window;
}

template <typename Container>
[[nodiscard]] std::vector<uint8_t> getSparseWindow(gzip::BitReader &bitReader,
                                                   const Container &window) {
  const auto usedSymbols = getUsedWindowSymbols(bitReader);
  std::vector<uint8_t> sparseWindow(std::min<size_t>(32_Ki, window.size()), 0);
  for (size_t i = 0; i < sparseWindow.size(); ++i) {
    if (usedSymbols[i + (usedSymbols.size() - sparseWindow.size())]) {
      sparseWindow[i] = window[i + (window.size() - sparseWindow.size())];
    }
  };
  return sparseWindow;
}
} // namespace rapidgzip::deflate
