#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include <CompressedVector.hpp>
#include <DecodedData.hpp>
#include <FasterVector.hpp>
#include <FileUtils.hpp>
#include <crc32.hpp>
#include <gzip.hpp>

namespace rapidgzip {

static constexpr size_t ALLOCATION_CHUNK_SIZE = 128_Ki;

struct ChunkData : public deflate::DecodedData {
  using BaseType = deflate::DecodedData;

  using Window = CompressedVector<FasterVector<uint8_t>>;
  using SharedWindow = std::shared_ptr<const Window>;
  using WindowView = VectorView<uint8_t>;

  struct Configuration {
    size_t splitChunkSize{std::numeric_limits<size_t>::max()};

    FileType fileType{FileType::NONE};
    bool crc32Enabled{true};
    std::optional<CompressionType> windowCompressionType;
    bool windowSparsity{true};

    size_t maxDecompressedChunkSize{std::numeric_limits<size_t>::max()};
    std::optional<char> newlineCharacter{};
  };

  struct Subchunk {
    [[nodiscard]] bool operator==(const Subchunk &other) const {
      return (encodedOffset == other.encodedOffset) &&
             (decodedOffset == other.decodedOffset) &&
             (encodedSize == other.encodedSize) &&
             (decodedSize == other.decodedSize) &&
             (newlineCount == other.newlineCount) &&
             (static_cast<bool>(window) == static_cast<bool>(other.window)) &&
             (!static_cast<bool>(window) || !static_cast<bool>(other.window) ||
              (*window == *other.window));
    }

    [[nodiscard]] bool
    hasBeenPostProcessed(const bool requireNewlineCount) const {
      return static_cast<bool>(window) && usedWindowSymbols.empty() &&
             (newlineCount.has_value() || !requireNewlineCount);
    }

  public:
    size_t encodedOffset{0};
    size_t decodedOffset{0};
    size_t encodedSize{0};
    size_t decodedSize{0};
    std::optional<size_t> newlineCount{};
    SharedWindow window{};
    std::vector<bool> usedWindowSymbols{};
  };

  class Statistics {
  public:
    void merge(const Statistics &other) {
      falsePositiveCount += other.falsePositiveCount;
      blockFinderDuration += other.blockFinderDuration;
      decodeDuration += other.decodeDuration;
      decodeDurationInflateWrapper += other.decodeDurationInflateWrapper;
      decodeDurationIsal += other.decodeDurationIsal;
      appendDuration += other.appendDuration;
      applyWindowDuration += other.applyWindowDuration;
      computeChecksumDuration += other.computeChecksumDuration;
      compressWindowDuration += other.compressWindowDuration;
      markerCount += other.markerCount;
      nonMarkerCount += other.nonMarkerCount;
      realMarkerCount += other.realMarkerCount;
    }

  public:
    size_t falsePositiveCount{0};
    double blockFinderDuration{0};
    double decodeDuration{0};
    double decodeDurationInflateWrapper{0};
    double decodeDurationIsal{0};
    double appendDuration{0};
    double applyWindowDuration{0};
    double computeChecksumDuration{0};
    double compressWindowDuration{0};
    uint64_t markerCount{0};
    uint64_t nonMarkerCount{0};
    uint64_t realMarkerCount{0};
  };

public:
  explicit ChunkData(const Configuration &configurationToUse)
      : configuration(configurationToUse) {
    setCRC32Enabled(configurationToUse.crc32Enabled);
  }

  ~ChunkData() = default;
  ChunkData() = default;
  ChunkData(ChunkData &&) = default;
  ChunkData(const ChunkData &) = delete;
  ChunkData &operator=(ChunkData &&) = default;
  ChunkData &operator=(const ChunkData &) = delete;

  [[nodiscard]] CompressionType windowCompressionType() const {
    if (configuration.windowCompressionType) {
      return *configuration.windowCompressionType;
    }

    return configuration.windowSparsity ||
                   (decodedSizeInBytes * 8 > 2 * encodedSizeInBits)
               ? CompressionType::ZLIB
               : CompressionType::NONE;
  }

  void append(deflate::DecodedVector &&toAppend) {
    auto t0 = now();

    if (crc32s.back().enabled()) {
      crc32s.back().update(toAppend.data(), toAppend.size());

      const auto t1 = now();
      statistics.computeChecksumDuration += duration(t0, t1);
      t0 = t1;
    }

    BaseType::append(std::move(toAppend));
    statistics.appendDuration += duration(t0);
  }

  void append(deflate::DecodedDataView const &toAppend) {
    auto t0 = now();

    if (crc32s.back().enabled()) {
      for (const auto &buffer : toAppend.data) {
        crc32s.back().update(buffer.data(), buffer.size());
      }

      const auto t1 = now();
      statistics.computeChecksumDuration += duration(t0, t1);
      t0 = t1;
    }

    BaseType::append(toAppend);
    statistics.appendDuration += duration(t0);
  }

  void applyWindow(WindowView const &window,
                   CompressionType const windowCompressionType) {
    const auto markerCount = dataWithMarkersSize();
    const auto tApplyStart = now();

    static constexpr bool ENABLE_REAL_MARKER_COUNT = false;
    if constexpr (ENABLE_REAL_MARKER_COUNT) {
      statistics.realMarkerCount += countMarkerSymbols();
    }

    BaseType::applyWindow(window);

    const auto tApplyEnd = now();
    if (markerCount > 0) {
      statistics.markerCount += markerCount;
      statistics.applyWindowDuration += duration(tApplyStart, tApplyEnd);
    }

    const auto alreadyProcessedSize =
        std::accumulate(crc32s.begin(), crc32s.end(), size_t(0),
                        [](const auto sum, const auto &crc32) {
                          return sum + crc32.streamSize();
                        });
    if (crc32s.front().enabled() &&
        (alreadyProcessedSize < BaseType::dataSize())) {

      const auto toProcessSize = BaseType::dataSize() - alreadyProcessedSize;
      CRC32Calculator crc32;
      for (auto it = DecodedData::Iterator(*this, 0, toProcessSize);
           static_cast<bool>(it); ++it) {
        const auto [buffer, size] = *it;
        crc32.update(buffer, size);
      }
      crc32s.front().prepend(crc32);

      statistics.computeChecksumDuration += duration(tApplyEnd);
    }

    const auto tWindowCompressionStart = now();
    size_t decodedOffsetInBlock{0};
    for (auto &subchunk : m_subchunks) {
      decodedOffsetInBlock += subchunk.decodedSize;

      if (!subchunk.window) {
        auto subchunkWindow = getWindowAt(window, decodedOffsetInBlock);

        if (subchunkWindow.size() == subchunk.usedWindowSymbols.size()) {
          for (size_t i = 0; i < subchunkWindow.size(); ++i) {
            if (!subchunk.usedWindowSymbols[i]) {
              subchunkWindow[i] = 0;
            }
          }
        }
        subchunk.window = std::make_shared<Window>(std::move(subchunkWindow),
                                                   windowCompressionType);
      }
      subchunk.usedWindowSymbols = std::vector<bool>();

      if (configuration.newlineCharacter && !subchunk.newlineCount) {
        size_t newlineCount = 0;
        using rapidgzip::deflate::DecodedData;
        for (auto it = DecodedData::Iterator(*this, subchunk.decodedOffset,
                                             subchunk.decodedSize);
             static_cast<bool>(it); ++it) {
          const auto &[buffer, size] = *it;
          newlineCount +=
              std::count(reinterpret_cast<const char *>(buffer),
                         reinterpret_cast<const char *>(buffer) + size,
                         configuration.newlineCharacter.value());
        }
        subchunk.newlineCount = newlineCount;
      }
    }
    statistics.compressWindowDuration += duration(tWindowCompressionStart);

    if (!hasBeenPostProcessed()) {
      std::stringstream message;
      message << "[Info] Chunk is not recognized as post-processed even though "
                 "it has been!\n"
              << "[Info]    Subchunks : " << m_subchunks.size() << "\n"
              << "[Info]    Contains markers : " << containsMarkers() << "\n";
      for (auto &subchunk : m_subchunks) {
        if (subchunk.hasBeenPostProcessed(
                configuration.newlineCharacter.has_value())) {
          continue;
        }
        message << "[Info] Subchunk is not recognized as post-processed even "
                   "though it has been!\n"
                << "[Info]    Has window : "
                << static_cast<bool>(subchunk.window) << "\n"
                << "[Info]    Used window symbols empty : "
                << subchunk.usedWindowSymbols.empty() << "\n"
                << "[Info]    Has newline count : "
                << subchunk.newlineCount.has_value() << "\n";
        if (configuration.newlineCharacter.has_value()) {
          message << "[Info]    Newline character : "
                  << static_cast<int>(subchunk.newlineCount.value()) << "\n";
        }
      }
#ifdef RAPIDGZIP_FATAL_PERFORMANCE_WARNINGS
      throw std::logic_error(std::move(message).str());
#else
      std::cerr << std::move(message).str();
#endif
    }
  }

  [[nodiscard]] bool matchesEncodedOffset(size_t offset) const noexcept {
    if (maxEncodedOffsetInBits == std::numeric_limits<size_t>::max()) {
      return offset == encodedOffsetInBits;
    }
    return (encodedOffsetInBits <= offset) &&
           (offset <= maxEncodedOffsetInBits);
  }

  void setEncodedOffset(size_t offset);

  void setSubchunks(std::vector<Subchunk> &&newSubchunks) {
    m_subchunks = std::move(newSubchunks);
  }

  void finalize(size_t newEncodedEndOffsetInBits) {
    const auto oldMarkerSize = BaseType::dataWithMarkersSize();
    cleanUnmarkedData();
    const auto toProcessSize = oldMarkerSize - BaseType::dataWithMarkersSize();
    if (toProcessSize > 0) {
      const auto tComputeHashStart = now();

      CRC32Calculator crc32;

      for (auto it = DecodedData::Iterator(*this, 0, toProcessSize);
           static_cast<bool>(it); ++it) {
        const auto [buffer, size] = *it;
        crc32.update(buffer, size);
      }

      crc32s.front().prepend(crc32);

      statistics.computeChecksumDuration += duration(tComputeHashStart);
    }

    statistics.nonMarkerCount += dataSize();

    encodedEndOffsetInBits = newEncodedEndOffsetInBits;
    encodedSizeInBits = newEncodedEndOffsetInBits - encodedOffsetInBits;
    decodedSizeInBytes = BaseType::size();

    if (m_subchunks.empty()) {
      m_subchunks = split(configuration.splitChunkSize);
    }
  }

  bool appendDeflateBlockBoundary(const size_t encodedOffset,
                                  const size_t decodedOffset) {
    if (blockBoundaries.empty() ||
        (blockBoundaries.back().encodedOffset != encodedOffset) ||
        (blockBoundaries.back().decodedOffset != decodedOffset)) {
      blockBoundaries.emplace_back(BlockBoundary{encodedOffset, decodedOffset});
      return true;
    }
    return false;
  }

  void appendFooter(const Footer &footer) {
    footers.emplace_back(footer);

    const auto wasEnabled = crc32s.back().enabled();
    crc32s.emplace_back();
    crc32s.back().setEnabled(wasEnabled);
  }

  void setCRC32Enabled(bool enabled) {
    configuration.crc32Enabled = enabled;
    for (auto &calculator : crc32s) {
      calculator.setEnabled(enabled);
    }
  }

  [[nodiscard]] bool hasBeenPostProcessed() const {
    return !m_subchunks.empty() && !containsMarkers() &&
           std::all_of(m_subchunks.begin(), m_subchunks.end(),
                       [this](const auto &subchunk) {
                         return subchunk.hasBeenPostProcessed(
                             configuration.newlineCharacter.has_value());
                       });
  }

  [[nodiscard]] const std::vector<Subchunk> &subchunks() const noexcept {
    return m_subchunks;
  }

  [[nodiscard]] constexpr size_t minimumSplitChunkSize() const {
    return configuration.splitChunkSize / 4U;
  }

  [[nodiscard]] deflate::DecodedVector
  getWindowAt(WindowView const &previousWindow, size_t skipBytes) const {
    return m_getWindowAt ? m_getWindowAt(*this, previousWindow, skipBytes)
                         : DecodedData::getWindowAt(previousWindow, skipBytes);
  }

protected:
  [[nodiscard]] std::vector<Subchunk> split(size_t spacing) const;

public:
  size_t encodedOffsetInBits{std::numeric_limits<size_t>::max()};
  size_t encodedSizeInBits{0};

  size_t maxEncodedOffsetInBits{std::numeric_limits<size_t>::max()};

  size_t decodedSizeInBytes{0};

  size_t encodedEndOffsetInBits{std::numeric_limits<size_t>::max()};

  Configuration configuration;

  std::vector<BlockBoundary> blockBoundaries;
  std::vector<Footer> footers;

  std::vector<CRC32Calculator> crc32s{std::vector<CRC32Calculator>(1)};

  Statistics statistics{};

  bool stoppedPreemptively{false};

protected:
  std::function<deflate::DecodedVector(const ChunkData &, WindowView const &,
                                       size_t)>
      m_getWindowAt;
  std::vector<Subchunk> m_subchunks;
};

inline std::ostream &operator<<(std::ostream &out, const ChunkData &chunk) {
  out << "ChunkData{\n";
  out << "  encodedOffsetInBits: " << chunk.encodedOffsetInBits << "\n";
  out << "  encodedSizeInBits: " << chunk.encodedSizeInBits << "\n";
  out << "  maxEncodedOffsetInBits: " << chunk.maxEncodedOffsetInBits << "\n";
  out << "  decodedSizeInBytes: " << chunk.decodedSizeInBytes << "\n";
  out << "  blockBoundaries: { ";
  for (const auto &boundary : chunk.blockBoundaries) {
    out << boundary.encodedOffset << ":" << boundary.decodedOffset << ", ";
  }
  out << "}\n";
  out << "  footers: { ";
  for (const auto &footer : chunk.footers) {
    out << footer.blockBoundary.encodedOffset << ":"
        << footer.blockBoundary.decodedOffset << ", ";
  }
  out << "}\n";
  out << "}\n";
  return out;
}

inline void ChunkData::setEncodedOffset(size_t offset) {
  if (!matchesEncodedOffset(offset)) {
    throw std::invalid_argument(
        "The real offset to correct to should lie inside the offset range!");
  }

  if (encodedEndOffsetInBits == std::numeric_limits<size_t>::max()) {
    throw std::invalid_argument(
        "Finalize must be called before setEncodedOffset!");
  }

  if (encodedEndOffsetInBits < offset) {
    std::stringstream message;
    message << "The chunk start " << offset
            << " must not be after the chunk end " << encodedEndOffsetInBits
            << "!";
    throw std::invalid_argument(std::move(message).str());
  }

  encodedSizeInBits = encodedEndOffsetInBits - offset;
  encodedOffsetInBits = offset;
  maxEncodedOffsetInBits = offset;

  if (!m_subchunks.empty()) {
    const auto nextSubchunk = std::next(m_subchunks.begin());
    const auto nextOffset = nextSubchunk == m_subchunks.end()
                                ? encodedEndOffsetInBits
                                : nextSubchunk->encodedOffset;
    m_subchunks.front().encodedOffset = offset;
    m_subchunks.front().encodedSize = nextOffset - offset;
  }
}

[[nodiscard]] inline std::vector<ChunkData::Subchunk>
ChunkData::split([[maybe_unused]] const size_t spacing) const {
  if (encodedEndOffsetInBits == std::numeric_limits<size_t>::max()) {
    throw std::invalid_argument(
        "Finalize must be called before splitting the chunk!");
  }

  if (spacing == 0) {
    throw std::invalid_argument("Spacing must be a positive number of bytes.");
  }

  if ((encodedSizeInBits == 0) && (decodedSizeInBytes == 0)) {
    return {};
  }

  const auto nBlocks = static_cast<size_t>(std::round(
      static_cast<double>(decodedSizeInBytes) / static_cast<double>(spacing)));
  Subchunk wholeChunkAsSubchunk;
  wholeChunkAsSubchunk.encodedOffset = encodedOffsetInBits;
  wholeChunkAsSubchunk.decodedOffset = 0;
  wholeChunkAsSubchunk.encodedSize = encodedSizeInBits;
  wholeChunkAsSubchunk.decodedSize = decodedSizeInBytes;

  if ((nBlocks <= 1) || blockBoundaries.empty()) {
    return {wholeChunkAsSubchunk};
  }

  const auto perfectSpacing =
      static_cast<double>(decodedSizeInBytes) / static_cast<double>(nBlocks);

  std::vector<Subchunk> result;
  result.reserve(nBlocks + 1);

  BlockBoundary lastBoundary{encodedOffsetInBits, 0};

  for (size_t iSubchunk = 1; iSubchunk < nBlocks; ++iSubchunk) {
    const auto perfectDecompressedOffset =
        static_cast<size_t>(static_cast<double>(iSubchunk) * perfectSpacing);

    const auto isCloser = [perfectDecompressedOffset](const auto &b1,
                                                      const auto &b2) {
      return absDiff(b1.decodedOffset, perfectDecompressedOffset) <
             absDiff(b2.decodedOffset, perfectDecompressedOffset);
    };
    auto closest = std::min_element(blockBoundaries.begin(),
                                    blockBoundaries.end(), isCloser);

    while ((std::next(closest) != blockBoundaries.end()) &&
           (closest->decodedOffset == std::next(closest)->decodedOffset)) {
      ++closest;
    }

    if (closest->decodedOffset <= lastBoundary.decodedOffset) {
      continue;
    }

    if (closest->encodedOffset <= lastBoundary.encodedOffset) {
      throw std::logic_error("If the decoded offset is strictly larger than so "
                             "must be the encoded one!");
    }

    Subchunk subchunk;
    subchunk.encodedOffset = lastBoundary.encodedOffset;
    subchunk.decodedOffset = result.empty() ? 0
                                            : result.back().decodedOffset +
                                                  result.back().decodedSize;
    subchunk.encodedSize = closest->encodedOffset - lastBoundary.encodedOffset;
    subchunk.decodedSize = closest->decodedOffset - lastBoundary.decodedOffset;
    result.emplace_back(subchunk);
    lastBoundary = *closest;
  }

  if (lastBoundary.decodedOffset > decodedSizeInBytes) {
    throw std::logic_error(
        "There should be no boundary outside of the chunk range!");
  }
  if ((lastBoundary.decodedOffset < decodedSizeInBytes) || result.empty()) {

    Subchunk subchunk;
    subchunk.encodedOffset = lastBoundary.encodedOffset,
    subchunk.decodedOffset = result.empty() ? 0
                                            : result.back().decodedOffset +
                                                  result.back().decodedSize;
    subchunk.encodedSize = encodedEndOffsetInBits - lastBoundary.encodedOffset,
    subchunk.decodedSize = decodedSizeInBytes - lastBoundary.decodedOffset,
    result.emplace_back(subchunk);
  } else if (lastBoundary.decodedOffset == decodedSizeInBytes) {

    result.back().encodedSize =
        encodedEndOffsetInBits - result.back().encodedOffset;
  }

  if (encodedEndOffsetInBits - encodedOffsetInBits != encodedSizeInBits) {
    std::stringstream message;
    message << "The offset: " << encodedOffsetInBits
            << ", size: " << encodedSizeInBits
            << ", and end offset: " << encodedEndOffsetInBits
            << " are inconsistent!";
    throw std::logic_error(std::move(message).str());
  }

  const auto subchunkEncodedSizeSum = std::accumulate(
      result.begin(), result.end(), size_t(0),
      [](size_t sum, const auto &block) { return sum + block.encodedSize; });
  const auto subchunkDecodedSizeSum = std::accumulate(
      result.begin(), result.end(), size_t(0),
      [](size_t sum, const auto &block) { return sum + block.decodedSize; });
  if ((subchunkEncodedSizeSum != encodedSizeInBits) ||
      (subchunkDecodedSizeSum != decodedSizeInBytes)) {
    std::stringstream message;
    message << "[Warning] Block splitting was unsuccessful. This might result "
               "in higher memory usage but is "
            << "otherwise harmless. Please report this performance bug with a "
               "reproducing example.\n"
            << "  subchunkEncodedSizeSum: " << subchunkEncodedSizeSum << "\n"
            << "  encodedSizeInBits     : " << encodedSizeInBits << "\n"
            << "  subchunkDecodedSizeSum: " << subchunkDecodedSizeSum << "\n"
            << "  decodedSizeInBytes    : " << decodedSizeInBytes << "\n";
#ifdef RAPIDGZIP_FATAL_PERFORMANCE_WARNINGS
    throw std::logic_error(std::move(message).str());
#else
    std::cerr << std::move(message).str();
#endif
    return {wholeChunkAsSubchunk};
  }

  return result;
}

static_assert(!std::is_polymorphic_v<ChunkData>,
              "Simply making it polymorphic halves performance!");

#if defined(HAVE_VMSPLICE)

[[nodiscard]] inline int
writeAllSplice([[maybe_unused]] const int outputFileDescriptor,
               [[maybe_unused]] const std::shared_ptr<ChunkData> &chunkData,
               [[maybe_unused]] const std::vector<::iovec> &buffersToWrite) {
  return SpliceVault::getInstance(outputFileDescriptor)
      .first->splice(buffersToWrite, chunkData);
}
#endif

[[nodiscard]] inline int writeAll(const std::shared_ptr<ChunkData> &chunkData,
                                  const int outputFileDescriptor,
                                  const size_t offsetInBlock,
                                  const size_t dataToWriteSize) {
  if ((outputFileDescriptor < 0) || (dataToWriteSize == 0)) {
    return 0;
  }

#ifdef HAVE_VMSPLICE
  const auto buffersToWrite =
      toIoVec(*chunkData, offsetInBlock, dataToWriteSize);
  const auto errorCode =
      writeAllSplice(outputFileDescriptor, chunkData, buffersToWrite);
  if (errorCode != 0) {
    return writeAllToFdVector(outputFileDescriptor, buffersToWrite);
  }
#else
  using rapidgzip::deflate::DecodedData;

  for (auto it =
           DecodedData::Iterator(*chunkData, offsetInBlock, dataToWriteSize);
       static_cast<bool>(it); ++it) {
    const auto &[buffer, size] = *it;
    const auto errorCode = writeAllToFd(outputFileDescriptor, buffer, size);
    if (errorCode != 0) {
      return errorCode;
    }
  }
#endif

  return 0;
}

struct ChunkDataCounter final : public ChunkData {
  explicit ChunkDataCounter(const Configuration &configurationToUse)
      : ChunkData(configurationToUse) {

    m_getWindowAt = [](const ChunkData &, WindowView const &, size_t) {
      return deflate::DecodedVector(deflate::MAX_WINDOW_SIZE, 0);
    };
  }

  ChunkDataCounter() {
    m_getWindowAt = [](const ChunkData &, WindowView const &, size_t) {
      return deflate::DecodedVector(deflate::MAX_WINDOW_SIZE, 0);
    };
  }

  ~ChunkDataCounter() = default;
  ChunkDataCounter(ChunkDataCounter &&) = default;
  ChunkDataCounter(const ChunkDataCounter &) = delete;
  ChunkDataCounter &operator=(ChunkDataCounter &&) = default;
  ChunkDataCounter &operator=(const ChunkDataCounter &) = delete;

  void append(deflate::DecodedVector &&toAppend) {
    decodedSizeInBytes += toAppend.size();
  }

  void append(deflate::DecodedDataView const &toAppend) {
    decodedSizeInBytes += toAppend.size();
  }

  void finalize(size_t newEncodedEndOffsetInBits) {
    encodedEndOffsetInBits = newEncodedEndOffsetInBits;
    encodedSizeInBits = encodedEndOffsetInBits - encodedOffsetInBits;

    m_subchunks = split(configuration.splitChunkSize);
  }

  [[nodiscard]] std::vector<Subchunk>
  split([[maybe_unused]] const size_t spacing) const {
    return ChunkData::split(std::numeric_limits<size_t>::max());
  }
};
} // namespace rapidgzip
