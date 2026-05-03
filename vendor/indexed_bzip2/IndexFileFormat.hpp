#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <Bgzf.hpp>
#include <FileReader.hpp>
#include <FileUtils.hpp>
#include <ThreadPool.hpp>
#include <VectorView.hpp>
#include <common.hpp>
#ifdef LIBRAPIDARCHIVE_WITH_ISAL
#include <isal.hpp>
#endif
#include <zlib.hpp>

#include "WindowMap.hpp"

namespace rapidgzip {

struct Checkpoint {
  uint64_t compressedOffsetInBits{0};
  uint64_t uncompressedOffsetInBytes{0};
  uint64_t lineOffset{0};

  [[nodiscard]] constexpr bool
  operator==(const Checkpoint &other) const noexcept {
    return (compressedOffsetInBits == other.compressedOffsetInBits) &&
           (uncompressedOffsetInBytes == other.uncompressedOffsetInBytes) &&
           (lineOffset == other.lineOffset);
  }
};

enum class IndexFormat {
  INDEXED_GZIP = 0,
  GZTOOL = 1,
  GZTOOL_WITH_LINES = 2,
};

enum class NewlineFormat {
  LINE_FEED = 0,
  CARRIAGE_RETURN = 1,
};

inline std::ostream &operator<<(std::ostream &out,
                                NewlineFormat newlineFormat) {
  switch (newlineFormat) {
  case NewlineFormat::LINE_FEED:
    out << "\\n";
    break;
  case NewlineFormat::CARRIAGE_RETURN:
    out << "\\r";
    break;
  }
  return out;
}

struct GzipIndex {
public:
  GzipIndex() = default;
  GzipIndex(GzipIndex &&) = default;
  GzipIndex &operator=(GzipIndex &&) = default;

  [[nodiscard]] GzipIndex clone() const {
    GzipIndex result(*this);
    if (windows) {
      result.windows = std::make_shared<WindowMap>(*windows);
    }
    return result;
  }

private:
  GzipIndex(const GzipIndex &) = default;
  GzipIndex &operator=(const GzipIndex &) = default;

public:
  uint64_t compressedSizeInBytes{std::numeric_limits<uint64_t>::max()};
  uint64_t uncompressedSizeInBytes{std::numeric_limits<uint64_t>::max()};

  uint32_t checkpointSpacing{0};
  uint32_t windowSizeInBytes{0};

  std::vector<Checkpoint> checkpoints;

  std::shared_ptr<WindowMap> windows;

  bool hasLineOffsets{false};
  NewlineFormat newlineFormat{NewlineFormat::LINE_FEED};

  [[nodiscard]] constexpr bool
  operator==(const GzipIndex &other) const noexcept {

    return (compressedSizeInBytes == other.compressedSizeInBytes) &&
           (uncompressedSizeInBytes == other.uncompressedSizeInBytes) &&
           (checkpointSpacing == other.checkpointSpacing) &&
           (windowSizeInBytes == other.windowSizeInBytes) &&
           (checkpoints == other.checkpoints) &&
           (hasLineOffsets == other.hasLineOffsets) &&
           (newlineFormat == other.newlineFormat) &&
           ((windows == other.windows) ||
            (windows && other.windows && (*windows == *other.windows)));
  }
};

inline std::ostream &operator<<(std::ostream &out, const GzipIndex &index) {
  out << "GzipIndex{\n";
  out << "  compressedSizeInBytes: " << index.compressedSizeInBytes << "\n";
  out << "  uncompressedSizeInBytes: " << index.uncompressedSizeInBytes << "\n";
  out << "  checkpointSpacing: " << index.checkpointSpacing << "\n";
  out << "  windowSizeInBytes: " << index.windowSizeInBytes << "\n";
  out << "  checkpoints: {\n    ";
  for (const auto &checkpoint : index.checkpoints) {
    out << checkpoint.compressedOffsetInBits << ":"
        << checkpoint.uncompressedOffsetInBytes << ", ";
  }
  out << "  }\n}\n";
  return out;
}

inline void checkedRead(FileReader *const indexFile, void *buffer,
                        size_t size) {
  if (indexFile == nullptr) {
    throw std::invalid_argument("Index file reader must be valid!");
  }
  const auto nBytesRead =
      indexFile->read(reinterpret_cast<char *>(buffer), size);
  if (nBytesRead != size) {
    throw std::runtime_error("Premature end of index file! Got only " +
                             std::to_string(nBytesRead) + " out of " +
                             std::to_string(size) + " requested bytes.");
  }
}

template <typename T> [[nodiscard]] T readValue(FileReader *const file) {

  T value;
  checkedRead(file, &value, sizeof(value));
  return value;
}

template <typename T>
[[nodiscard]] T readBigEndianValue(FileReader *const file) {
  T value;
  checkedRead(file, &value, sizeof(value));
  if (ENDIAN == Endian::LITTLE) {
    auto *const buffer = reinterpret_cast<char *>(&value);
    for (size_t i = 0; i < sizeof(T) / 2; ++i) {
      std::swap(buffer[i], buffer[sizeof(T) - 1 - i]);
    }
  }
  return value;
}

namespace bgzip {
[[nodiscard]] inline size_t
countDecompressedBytes(gzip::BitReader &&bitReader,
                       VectorView<std::uint8_t> const initialWindow) {
#ifdef LIBRAPIDARCHIVE_WITH_ISAL
  using InflateWrapper = rapidgzip::IsalInflateWrapper;
#else
  using InflateWrapper = rapidgzip::ZlibInflateWrapper;
#endif

  InflateWrapper inflateWrapper(std::move(bitReader),
                                std::numeric_limits<size_t>::max());
  inflateWrapper.setWindow(initialWindow);

  size_t alreadyDecoded{0};
  std::vector<uint8_t> subchunk(128_Ki);
  while (true) {
    std::optional<rapidgzip::Footer> footer;
    size_t nBytesReadPerCall{0};
    while (!footer) {
      std::tie(nBytesReadPerCall, footer) =
          inflateWrapper.readStream(subchunk.data(), subchunk.size());
      if (nBytesReadPerCall == 0) {
        break;
      }
      alreadyDecoded += nBytesReadPerCall;
    }

    if ((nBytesReadPerCall == 0) && !footer) {
      break;
    }
  }

  return alreadyDecoded;
}

[[nodiscard]] inline GzipIndex
readGzipIndex(UniqueFileReader indexFile, UniqueFileReader archiveFile = {},
              const std::vector<char> &alreadyReadBytes = {}) {
  if (!indexFile) {
    throw std::invalid_argument("Index file reader must be valid!");
  }
  if (alreadyReadBytes.size() != indexFile->tell()) {
    throw std::invalid_argument(
        "The file position must match the number of given bytes.");
  }
  static constexpr size_t MAGIC_BYTE_COUNT = sizeof(uint64_t);
  if (alreadyReadBytes.size() > MAGIC_BYTE_COUNT) {
    throw std::invalid_argument("This function only supports skipping up to "
                                "over the magic bytes if given.");
  }

  if (!archiveFile || !archiveFile->size().has_value()) {
    throw std::invalid_argument(
        "Cannot import bgzip index without knowing the archive size!");
  }
  const auto archiveSize = archiveFile->size();

  uint64_t numberOfEntries{0};
  std::memcpy(&numberOfEntries, alreadyReadBytes.data(),
              alreadyReadBytes.size());
  checkedRead(indexFile.get(),
              reinterpret_cast<char *>(&numberOfEntries) +
                  alreadyReadBytes.size(),
              sizeof(uint64_t) - alreadyReadBytes.size());

  GzipIndex index;

  if (numberOfEntries == std::numeric_limits<uint64_t>::max()) {
    numberOfEntries = 0;

    index.compressedSizeInBytes = 0;
    index.uncompressedSizeInBytes = 0;
  }

  const auto expectedFileSize = (2U * numberOfEntries + 1U) * sizeof(uint64_t);
  if ((indexFile->size() > 0) && (indexFile->size() != expectedFileSize)) {
    throw std::invalid_argument("Invalid magic bytes!");
  }
  index.compressedSizeInBytes = *archiveSize;

  index.checkpoints.reserve(numberOfEntries + 1);

  const auto sharedArchiveFile = ensureSharedFileReader(std::move(archiveFile));

  try {
    rapidgzip::blockfinder::Bgzf blockfinder(sharedArchiveFile->clone());
    const auto firstBlockOffset = blockfinder.find();
    if (firstBlockOffset == std::numeric_limits<size_t>::max()) {
      throw std::invalid_argument("");
    }

    auto &firstCheckPoint = index.checkpoints.emplace_back();
    firstCheckPoint.compressedOffsetInBits = firstBlockOffset;
    firstCheckPoint.uncompressedOffsetInBytes = 0;
  } catch (const std::invalid_argument &exception) {
    std::stringstream message;
    message << "Trying to load a BGZF index for a non-BGZF file!";
    const std::string_view what(exception.what());
    if (!what.empty()) {
      message << " (" << what << ")";
    }
    throw std::invalid_argument(std::move(message).str());
  }

  index.windows = std::make_shared<WindowMap>();

  for (uint64_t i = 1; i < numberOfEntries; ++i) {
    auto &checkpoint = index.checkpoints.emplace_back();
    checkpoint.compressedOffsetInBits = readValue<uint64_t>(indexFile.get());
    checkpoint.uncompressedOffsetInBytes = readValue<uint64_t>(indexFile.get());
    checkpoint.compressedOffsetInBits += 18U;
    checkpoint.compressedOffsetInBits *= 8U;

    const auto &lastCheckPoint = *(index.checkpoints.rbegin() + 1);

    if (checkpoint.compressedOffsetInBits > index.compressedSizeInBytes * 8U) {
      std::stringstream message;
      message << "Compressed bit offset (" << checkpoint.compressedOffsetInBits
              << ") should be smaller or equal than the file size ("
              << index.compressedSizeInBytes * 8U << ")!";
      throw std::invalid_argument(std::move(message).str());
    }

    if (checkpoint.compressedOffsetInBits <=
        lastCheckPoint.compressedOffsetInBits) {
      std::stringstream message;
      message << "Compressed bit offset (" << checkpoint.compressedOffsetInBits
              << ") should be greater than predecessor ("
              << lastCheckPoint.compressedOffsetInBits << ")!";
      throw std::invalid_argument(std::move(message).str());
    }

    if (checkpoint.uncompressedOffsetInBytes <
        lastCheckPoint.uncompressedOffsetInBytes) {
      std::stringstream message;
      message << "Uncompressed offset (" << checkpoint.uncompressedOffsetInBytes
              << ") should be greater or equal than predecessor ("
              << lastCheckPoint.uncompressedOffsetInBytes << ")!";
      throw std::invalid_argument(std::move(message).str());
    }

    index.windows->emplace(checkpoint.compressedOffsetInBits, {},
                           CompressionType::NONE);
  }

  try {
    gzip::BitReader bitReader(sharedArchiveFile->clone());
    bitReader.seekTo(index.checkpoints.back().compressedOffsetInBits);
    index.uncompressedSizeInBytes =
        index.checkpoints.back().uncompressedOffsetInBytes

        + countDecompressedBytes(std::move(bitReader), {});
  } catch (const std::invalid_argument &) {
    throw std::invalid_argument(
        "Unable to read from the last given offset in the index!");
  }

  return index;
}
} // namespace bgzip

namespace indexed_gzip {
static constexpr std::string_view MAGIC_BYTES{"GZIDX"};

[[nodiscard]] inline GzipIndex
readGzipIndex(UniqueFileReader indexFile,
              const std::optional<size_t> archiveSize = std::nullopt,
              const std::vector<char> &alreadyReadBytes = {},
              size_t parallelization = 1) {
  if (!indexFile) {
    throw std::invalid_argument("Index file reader must be valid!");
  }
  if (alreadyReadBytes.size() != indexFile->tell()) {
    throw std::invalid_argument(
        "The file position must match the number of given bytes.");
  }
  static constexpr size_t HEADER_BUFFER_SIZE = MAGIC_BYTES.size() + 1U + 1U +
                                               2 * sizeof(uint64_t) +
                                               2 * sizeof(uint32_t);
  if (alreadyReadBytes.size() > HEADER_BUFFER_SIZE) {
    throw std::invalid_argument("This function only supports skipping up to "
                                "over the magic bytes if given.");
  }

  auto headerBytes = alreadyReadBytes;
  if (headerBytes.size() < HEADER_BUFFER_SIZE) {
    const auto oldSize = headerBytes.size();
    headerBytes.resize(HEADER_BUFFER_SIZE);
    checkedRead(indexFile.get(), headerBytes.data() + oldSize,
                headerBytes.size() - oldSize);
  }

  if (!std::equal(MAGIC_BYTES.begin(), MAGIC_BYTES.end(),
                  headerBytes.begin())) {
    throw std::invalid_argument("Magic bytes do not match! Expected 'GZIDX'.");
  }

  const auto headerBytesReader = std::make_unique<BufferViewFileReader>(
      headerBytes.data(), headerBytes.size());
  headerBytesReader->seekTo(MAGIC_BYTES.size());
  const auto formatVersion = readValue<uint8_t>(headerBytesReader.get());
  if (formatVersion > 1) {
    throw std::invalid_argument(
        "Index was written with a newer indexed_gzip version than supported!");
  }

  headerBytesReader->seek(1, SEEK_CUR);

  GzipIndex index;
  index.compressedSizeInBytes = readValue<uint64_t>(headerBytesReader.get());
  index.uncompressedSizeInBytes = readValue<uint64_t>(headerBytesReader.get());
  index.checkpointSpacing = readValue<uint32_t>(headerBytesReader.get());
  index.windowSizeInBytes = readValue<uint32_t>(headerBytesReader.get());

  indexFile->seekTo(HEADER_BUFFER_SIZE);

  if (archiveSize && (*archiveSize != index.compressedSizeInBytes)) {
    std::stringstream message;
    message << "File size for the compressed file (" << *archiveSize
            << ") does not fit the size stored in the given index ("
            << index.compressedSizeInBytes << ")!";
    throw std::invalid_argument(std::move(message).str());
  }

  if (index.windowSizeInBytes != 32_Ki) {
    throw std::invalid_argument(
        "Only a window size of 32 KiB makes sense because indexed_gzip "
        "supports "
        "no smaller ones and gzip does support any larger one.");
  }
  const auto checkpointCount = readValue<uint32_t>(indexFile.get());

  std::vector<std::tuple<size_t, size_t, double>> windowInfos;

  index.checkpoints.resize(checkpointCount);
  for (uint32_t i = 0; i < checkpointCount; ++i) {
    auto &checkpoint = index.checkpoints[i];

    checkpoint.compressedOffsetInBits = readValue<uint64_t>(indexFile.get());
    if (checkpoint.compressedOffsetInBits > index.compressedSizeInBytes) {
      throw std::invalid_argument(
          "Checkpoint compressed offset is after the file end!");
    }
    checkpoint.compressedOffsetInBits *= 8;

    checkpoint.uncompressedOffsetInBytes = readValue<uint64_t>(indexFile.get());
    if (checkpoint.uncompressedOffsetInBytes > index.uncompressedSizeInBytes) {
      throw std::invalid_argument(
          "Checkpoint uncompressed offset is after the file end!");
    }

    const auto bits = readValue<uint8_t>(indexFile.get());
    if (bits >= 8) {
      throw std::invalid_argument(
          "Denormal compressed offset for checkpoint. Bit offset >= 8!");
    }
    if (bits > 0) {
      if (checkpoint.compressedOffsetInBits == 0) {
        throw std::invalid_argument(
            "Denormal bits for checkpoint. Effectively negative offset!");
      }
      checkpoint.compressedOffsetInBits -= bits;
    }

    size_t windowSize{0};
    if (formatVersion == 0) {
      if (i != 0) {
        windowSize = index.windowSizeInBytes;
      }
    } else {
      if (readValue<uint8_t>(indexFile.get()) != 0) {
        windowSize = index.windowSizeInBytes;
      }
    }

    auto compressionRatio = 1.0;
    if (i >= 1) {
      const auto &previousCheckpoint = index.checkpoints[i - 1];
      compressionRatio =
          static_cast<double>(checkpoint.uncompressedOffsetInBytes -
                              previousCheckpoint.uncompressedOffsetInBytes) *
          8 /
          static_cast<double>(checkpoint.compressedOffsetInBits -
                              previousCheckpoint.compressedOffsetInBits);
    }
    windowInfos.emplace_back(checkpoint.compressedOffsetInBits, windowSize,
                             compressionRatio);
  }

  const auto backgroundThreadCount = parallelization == 1 ? 0 : parallelization;
  ThreadPool threadPool{backgroundThreadCount};
  std::deque<std::future<std::pair<size_t, std::shared_ptr<WindowMap::Window>>>>
      futures;

  const auto processFuture = [&]() {
    using namespace std::chrono_literals;

    if (futures.empty()) {
      return;
    }

    const auto oldSize = futures.size();
    for (auto it = futures.begin(); it != futures.end();) {
      auto &future = *it;
      if (!future.valid() ||
          (future.wait_for(0s) == std::future_status::ready)) {
        auto result = future.get();
        index.windows->emplaceShared(result.first, std::move(result.second));
        it = futures.erase(it);
      } else {
        ++it;
      }
    }

    if (futures.size() >= oldSize) {
      auto result = futures.front().get();
      index.windows->emplaceShared(result.first, std::move(result.second));
      futures.pop_front();
    }
  };

  index.windows = std::make_shared<WindowMap>();
  for (auto &[offset, windowSize, compressionRatio] : windowInfos) {

    auto window = std::make_shared<FasterVector<uint8_t>>();
    if (windowSize > 0) {
      window->resize(windowSize);
      checkedRead(indexFile.get(), window->data(), window->size());
    }

    if (compressionRatio > 2) {
      futures.emplace_back(threadPool.submit(
          [toCompress = std::move(window), offset2 = offset]() mutable {
            return std::make_pair(
                offset2, std::make_shared<WindowMap::Window>(
                             std::move(*toCompress), CompressionType::ZLIB));
          }));
      if (futures.size() >= 2 * backgroundThreadCount) {
        processFuture();
      }
    } else {
      index.windows->emplaceShared(
          offset, std::make_shared<WindowMap::Window>(std::move(*window),
                                                      CompressionType::NONE));
    }
  }

  while (!futures.empty()) {
    processFuture();
  }

  return index;
}

inline void writeGzipIndex(
    const GzipIndex &index,
    const std::function<void(const void *buffer, size_t size)> &checkedWrite) {
  const auto writeValue = [&checkedWrite](auto value) {
    checkedWrite(&value, sizeof(value));
  };

  const auto &checkpoints = index.checkpoints;
  const auto windowSizeInBytes = static_cast<uint32_t>(32_Ki);
  const auto hasValidWindow = [&index,
                               windowSizeInBytes](const auto &checkpoint) {
    if (checkpoint.compressedOffsetInBits == index.compressedSizeInBytes * 8U) {

      return true;
    }
    const auto window = index.windows->get(checkpoint.compressedOffsetInBits);
    return window && (window->empty() ||
                      (window->decompressedSize() >= windowSizeInBytes));
  };

  if (!std::all_of(checkpoints.begin(), checkpoints.end(), hasValidWindow)) {
    throw std::invalid_argument(
        "All window sizes must be at least 32 KiB or empty!");
  }

  checkedWrite(MAGIC_BYTES.data(), MAGIC_BYTES.size());
  checkedWrite("\x01", 1);
  checkedWrite("\x00", 1);

  uint32_t checkpointSpacing = index.checkpointSpacing;

  if (!checkpoints.empty() && (checkpointSpacing < windowSizeInBytes)) {
    std::vector<uint64_t> uncompressedOffsets(checkpoints.size());
    std::transform(checkpoints.begin(), checkpoints.end(),
                   uncompressedOffsets.begin(), [](const auto &checkpoint) {
                     return checkpoint.uncompressedOffsetInBytes;
                   });
    std::adjacent_difference(uncompressedOffsets.begin(),
                             uncompressedOffsets.end(),
                             uncompressedOffsets.begin());
    const auto minSpacing = std::accumulate(
        uncompressedOffsets.begin() + 1, uncompressedOffsets.end(), uint64_t(0),
        [](auto a, auto b) { return std::min(a, b); });
    checkpointSpacing =
        std::max(windowSizeInBytes, static_cast<uint32_t>(minSpacing));
  }

  writeValue(static_cast<uint64_t>(index.compressedSizeInBytes));
  writeValue(static_cast<uint64_t>(index.uncompressedSizeInBytes));
  writeValue(static_cast<uint32_t>(checkpointSpacing));
  writeValue(static_cast<uint32_t>(windowSizeInBytes));
  writeValue(static_cast<uint32_t>(checkpoints.size()));

  for (const auto &checkpoint : checkpoints) {
    const auto bits = checkpoint.compressedOffsetInBits % 8;
    writeValue(static_cast<uint64_t>(checkpoint.compressedOffsetInBits / 8 +
                                     (bits == 0 ? 0 : 1)));
    writeValue(static_cast<uint64_t>(checkpoint.uncompressedOffsetInBytes));
    writeValue(static_cast<uint8_t>(bits == 0 ? 0 : 8 - bits));

    const auto isLastWindow =
        checkpoint.compressedOffsetInBits == index.compressedSizeInBytes * 8U;
    const auto result = index.windows->get(checkpoint.compressedOffsetInBits);
    if (!result && !isLastWindow) {
      throw std::logic_error("Did not find window to offset " +
                             formatBits(checkpoint.compressedOffsetInBits));
    }
    writeValue(static_cast<uint8_t>(!result || result->empty() ? 0 : 1));
  }

  for (const auto &checkpoint : checkpoints) {
    const auto result = index.windows->get(checkpoint.compressedOffsetInBits);
    if (!result) {

      continue;
    }

    const auto windowPointer = result->decompress();
    if (!windowPointer) {
      continue;
    }

    const auto &window = *windowPointer;
    if (window.empty()) {
      continue;
    }

    if (window.size() == windowSizeInBytes) {
      checkedWrite(window.data(), window.size());
    } else if (window.size() > windowSizeInBytes) {
      checkedWrite(window.data() + window.size() - windowSizeInBytes,
                   windowSizeInBytes);
    } else if (window.size() < windowSizeInBytes) {
      const std::vector<char> zeros(windowSizeInBytes - window.size(), 0);
      checkedWrite(zeros.data(), zeros.size());
      checkedWrite(window.data(), window.size());
    }
  }
}
} // namespace indexed_gzip

namespace gztool {

static constexpr std::string_view MAGIC_BYTES{"\0\0\0\0\0\0\0\0gzipind",
                                              8U + 7U};

[[nodiscard]] inline GzipIndex
readGzipIndex(UniqueFileReader indexFile,
              const std::optional<size_t> archiveSize = {},
              const std::vector<char> &alreadyReadBytes = {}) {
  if (!indexFile) {
    throw std::invalid_argument("Index file reader must be valid!");
  }
  if (alreadyReadBytes.size() != indexFile->tell()) {
    throw std::invalid_argument(
        "The file position must match the number of given bytes.");
  }
  static constexpr size_t HEADER_BUFFER_SIZE = MAGIC_BYTES.size() + 1U;
  if (alreadyReadBytes.size() > HEADER_BUFFER_SIZE) {
    throw std::invalid_argument("This function only supports skipping up to "
                                "over the magic bytes if given.");
  }

  GzipIndex index;

  if (!archiveSize) {
    throw std::invalid_argument(
        "Cannot import gztool index without knowing the archive size!");
  }
  index.compressedSizeInBytes = archiveSize.value();

  auto headerBytes = alreadyReadBytes;
  if (headerBytes.size() < HEADER_BUFFER_SIZE) {
    const auto oldSize = headerBytes.size();
    headerBytes.resize(HEADER_BUFFER_SIZE);
    checkedRead(indexFile.get(), headerBytes.data() + oldSize,
                headerBytes.size() - oldSize);
  }

  if (!std::equal(MAGIC_BYTES.begin(), MAGIC_BYTES.end(),
                  headerBytes.begin())) {
    throw std::invalid_argument("Magic bytes do not match!");
  }

  if ((headerBytes.back() != 'x') && (headerBytes.back() != 'X')) {
    throw std::invalid_argument("Invalid index version. Expected 'x' or 'X'!");
  }
  const auto formatVersion = headerBytes.back() == 'x' ? 0 : 1;
  if (formatVersion > 1) {
    throw std::invalid_argument(
        "Index was written with a newer indexed_gzip version than supported!");
  }

  index.hasLineOffsets = formatVersion == 1;
  if (index.hasLineOffsets) {
    const auto format = readBigEndianValue<uint32_t>(indexFile.get());
    if (format > 1) {
      throw std::invalid_argument("Expected 0 or 1 for newline format!");
    }
    index.newlineFormat =
        format == 0 ? NewlineFormat::LINE_FEED : NewlineFormat::CARRIAGE_RETURN;
  }

  const auto checkpointCount = readBigEndianValue<uint64_t>(indexFile.get());
  const auto indexIsComplete =
      checkpointCount == readBigEndianValue<uint64_t>(indexFile.get());
  if (!indexIsComplete) {
    throw std::invalid_argument(
        "Reading an incomplete index is not supported!");
  }

  index.windows = std::make_shared<WindowMap>();

  std::array<uint8_t, rapidgzip::deflate::MAX_WINDOW_SIZE> decompressedWindow{};

  index.checkpoints.resize(checkpointCount);
  for (uint32_t i = 0; i < checkpointCount; ++i) {
    auto &checkpoint = index.checkpoints[i];

    checkpoint.uncompressedOffsetInBytes =
        readBigEndianValue<uint64_t>(indexFile.get());
    if (checkpoint.uncompressedOffsetInBytes > index.uncompressedSizeInBytes) {
      throw std::invalid_argument(
          "Checkpoint uncompressed offset is after the file end!");
    }

    checkpoint.compressedOffsetInBits =
        readBigEndianValue<uint64_t>(indexFile.get());
    if (checkpoint.compressedOffsetInBits > index.compressedSizeInBytes) {
      throw std::invalid_argument(
          "Checkpoint compressed offset is after the file end!");
    }
    checkpoint.compressedOffsetInBits *= 8;

    const auto bits = readBigEndianValue<uint32_t>(indexFile.get());
    if (bits >= 8) {
      throw std::invalid_argument(
          "Denormal compressed offset for checkpoint. Bit offset >= 8!");
    }
    if (bits > 0) {
      if (checkpoint.compressedOffsetInBits == 0) {
        throw std::invalid_argument(
            "Denormal bits for checkpoint. Effectively negative offset!");
      }
      checkpoint.compressedOffsetInBits -= bits;
    }

    const auto compressedWindowSize =
        readBigEndianValue<uint32_t>(indexFile.get());
    if (compressedWindowSize == 0) {

      index.windows->emplace(checkpoint.compressedOffsetInBits, {},
                             CompressionType::NONE);
    } else {
      FasterVector<uint8_t> compressedWindow(compressedWindowSize);
      checkedRead(indexFile.get(), compressedWindow.data(),
                  compressedWindow.size());

      gzip::BitReader bitReader(std::make_unique<BufferViewFileReader>(
          compressedWindow.data(), compressedWindow.size()));

#ifdef LIBRAPIDARCHIVE_WITH_ISAL
      using InflateWrapper = rapidgzip::IsalInflateWrapper;
#else
      using InflateWrapper = rapidgzip::ZlibInflateWrapper;
#endif

      InflateWrapper inflateWrapper(std::move(bitReader));
      inflateWrapper.setFileType(rapidgzip::FileType::ZLIB);
      inflateWrapper.setStartWithHeader(true);

      const auto [decompressedWindowSize, footer] = inflateWrapper.readStream(
          decompressedWindow.data(), decompressedWindow.size());
      if (!footer) {
        throw std::invalid_argument(
            "Expected zlib footer after at least 32 KiB of data!");
      }

      index.windows->emplaceShared(
          checkpoint.compressedOffsetInBits,
          std::make_shared<WindowMap::Window>(std::move(compressedWindow),
                                              decompressedWindowSize,
                                              CompressionType::ZLIB));
    }

    if (index.hasLineOffsets) {
      checkpoint.lineOffset = readBigEndianValue<uint64_t>(indexFile.get());
      if (checkpoint.lineOffset == 0) {
        throw std::invalid_argument(
            "Line number in gztool index is expected to be >0 by definition!");
      }
      checkpoint.lineOffset -= 1;
    }
  }

  if (indexIsComplete) {
    index.uncompressedSizeInBytes =
        readBigEndianValue<uint64_t>(indexFile.get());
    if (index.hasLineOffsets) {
      if (index.checkpoints.empty() ||
          (index.checkpoints.back().compressedOffsetInBits !=
           index.compressedSizeInBytes * 8U)) {
        auto &checkpoint = index.checkpoints.emplace_back();
        checkpoint.compressedOffsetInBits = index.compressedSizeInBytes * 8U;
        checkpoint.uncompressedOffsetInBytes = index.uncompressedSizeInBytes;

        index.windows->emplace(checkpoint.compressedOffsetInBits, {},
                               CompressionType::NONE);
      } else if (index.checkpoints.back().uncompressedOffsetInBytes !=
                 index.uncompressedSizeInBytes) {
        throw std::domain_error("The last checkpoint at the end of the "
                                "compressed stream does not match "
                                "the uncompressed size!");
      }
      index.checkpoints.back().lineOffset =
          readBigEndianValue<uint64_t>(indexFile.get());
    }
  }

  return index;
}

inline void writeGzipIndex(
    const GzipIndex &index,
    const std::function<void(const void *buffer, size_t size)> &checkedWrite) {
  const auto writeValue = [&checkedWrite](auto value) {
    if (ENDIAN == Endian::BIG) {
      checkedWrite(&value, sizeof(value));
    } else {
      std::array<char, sizeof(value)> buffer{};
      auto *const src = reinterpret_cast<char *>(&value);
      for (size_t i = 0; i < sizeof(value); ++i) {
        buffer[buffer.size() - 1 - i] = src[i];
      }
      checkedWrite(buffer.data(), buffer.size());
    }
  };

  const auto &checkpoints = index.checkpoints;
  const auto windowSizeInBytes = static_cast<uint32_t>(32_Ki);
  const auto hasValidWindow = [&index,
                               windowSizeInBytes](const auto &checkpoint) {
    if (checkpoint.compressedOffsetInBits == index.compressedSizeInBytes * 8U) {

      return true;
    }
    const auto window = index.windows->get(checkpoint.compressedOffsetInBits);
    return window && (window->empty() ||
                      (window->decompressedSize() >= windowSizeInBytes));
  };

  if (!std::all_of(checkpoints.begin(), checkpoints.end(), hasValidWindow)) {
    throw std::invalid_argument(
        "All window sizes must be at least 32 KiB or empty!");
  }

  checkedWrite(MAGIC_BYTES.data(), MAGIC_BYTES.size());
  checkedWrite(index.hasLineOffsets ? "X" : "x", 1);
  if (index.hasLineOffsets) {
    writeValue(static_cast<uint32_t>(
        index.newlineFormat == NewlineFormat::LINE_FEED ? 0 : 1));
  }

  auto lastCheckPoint = index.checkpoints.rbegin();
  while ((lastCheckPoint != index.checkpoints.rend()) &&
         (lastCheckPoint->uncompressedOffsetInBytes ==
          index.uncompressedSizeInBytes)) {
    ++lastCheckPoint;
  }
  const auto checkpointCount =
      index.checkpoints.size() -
      std::distance(index.checkpoints.rbegin(), lastCheckPoint);
  writeValue(static_cast<uint64_t>(checkpointCount));
  writeValue(static_cast<uint64_t>(checkpointCount));

  for (const auto &checkpoint : checkpoints) {
    if (checkpoint.compressedOffsetInBits == index.compressedSizeInBytes * 8U) {
      continue;
    }

    const auto bits = checkpoint.compressedOffsetInBits % 8;
    writeValue(static_cast<uint64_t>(checkpoint.uncompressedOffsetInBytes));
    writeValue(static_cast<uint64_t>(checkpoint.compressedOffsetInBits / 8 +
                                     (bits == 0 ? 0 : 1)));
    writeValue(static_cast<uint32_t>(bits == 0 ? 0 : 8 - bits));

    const auto result = index.windows->get(checkpoint.compressedOffsetInBits);
    if (!result) {
      throw std::logic_error("Did not find window to offset " +
                             formatBits(checkpoint.compressedOffsetInBits));
    }
    if (result->empty()) {
      writeValue(uint32_t(0));
    } else if (result->compressionType() == CompressionType::ZLIB) {
      writeValue(static_cast<uint32_t>(result->compressedSize()));
      const auto compressedData = result->compressedData();
      if (!compressedData) {
        throw std::logic_error("Did not get compressed data buffer!");
      }
      checkedWrite(compressedData->data(), compressedData->size());
    } else {

      const auto windowPointer = result->decompress();
      if (!windowPointer) {
        throw std::logic_error("Did not get decompressed data buffer!");
      }

      const auto &window = *windowPointer;
      if (window.empty()) {
        continue;
      }

      using namespace rapidgzip;
      const auto recompressed = compressWithZlib(
          window, CompressionStrategy::DEFAULT, {}, ContainerFormat::ZLIB);
      writeValue(static_cast<uint32_t>(recompressed.size()));
      checkedWrite(recompressed.data(), recompressed.size());
    }

    if (index.hasLineOffsets) {
      writeValue(static_cast<uint64_t>(checkpoint.lineOffset + 1U));
    }
  }

  writeValue(static_cast<uint64_t>(index.uncompressedSizeInBytes));
  if (index.hasLineOffsets) {
    writeValue(static_cast<uint64_t>(
        index.checkpoints.empty() ? 0
                                  : index.checkpoints.rbegin()->lineOffset));
  }
}
} // namespace gztool

[[nodiscard]] inline GzipIndex readGzipIndex(UniqueFileReader indexFile,
                                             UniqueFileReader archiveFile = {},
                                             size_t parallelization = 1) {
  std::vector<char> formatId(8, 0);
  checkedRead(indexFile.get(), formatId.data(), formatId.size());

  std::optional<size_t> archiveSize;
  if (archiveFile) {
    archiveSize = archiveFile->size();
  }

  if (const auto commonSize =
          std::min(formatId.size(), indexed_gzip::MAGIC_BYTES.size());
      std::string_view(formatId.data(), commonSize) ==
      std::string_view(indexed_gzip::MAGIC_BYTES.data(), commonSize)) {
    return indexed_gzip::readGzipIndex(std::move(indexFile), archiveSize,
                                       formatId, parallelization);
  }

  if (const auto commonSize =
          std::min(formatId.size(), gztool::MAGIC_BYTES.size());
      std::string_view(formatId.data(), commonSize) ==
      std::string_view(gztool::MAGIC_BYTES.data(), commonSize)) {
    return gztool::readGzipIndex(std::move(indexFile), archiveSize, formatId);
  }

  return bgzip::readGzipIndex(std::move(indexFile), std::move(archiveFile),
                              formatId);
}
} // namespace rapidgzip
