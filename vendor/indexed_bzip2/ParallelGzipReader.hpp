#pragma once

#include <algorithm>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <AffinityHelpers.hpp>
#include <BlockMap.hpp>
#include <FileReader.hpp>
#include <Shared.hpp>
#include <common.hpp>

#ifdef WITH_PYTHON_SUPPORT
#include <Python.hpp>
#include <Standard.hpp>
#endif

#include "GzipChunkFetcher.hpp"
#include "IndexFileFormat.hpp"
#include "crc32.hpp"
#include "gzip.hpp"

namespace rapidgzip {
#ifdef WITH_PYTHON_SUPPORT
enum class IOReadMethod : uint8_t {
  SEQUENTIAL = 0,
  PREAD = 1,
  LOCKED_READ_AND_SEEK = 2,
};

[[nodiscard]] static UniqueFileReader
wrapFileReader(UniqueFileReader &&fileReader, IOReadMethod ioReadMethod) {
  switch (ioReadMethod) {
  case IOReadMethod::SEQUENTIAL:
    return std::make_unique<SinglePassFileReader>(std::move(fileReader));
  case IOReadMethod::PREAD:
  case IOReadMethod::LOCKED_READ_AND_SEEK: {
    auto sharedFile = ensureSharedFileReader(std::move(fileReader));
    sharedFile->setUsePread(ioReadMethod == IOReadMethod::PREAD);
    return sharedFile;
  }
  }
  return std::move(fileReader);
}
#endif

template <typename T_ChunkData = ChunkData>
class ParallelGzipReader final : public FileReader {
public:
  using ChunkData = T_ChunkData;
  using ChunkConfiguration = typename ChunkData::Configuration;

  using ChunkFetcher =
      rapidgzip::GzipChunkFetcher<FetchingStrategy::FetchMultiStream,
                                  ChunkData>;
  using BlockFinder = typename ChunkFetcher::BlockFinder;
  using BitReader = gzip::BitReader;
  using WriteFunctor =
      std::function<void(const std::shared_ptr<ChunkData> &, size_t, size_t)>;
  using Window = WindowMap::Window;

  struct NewlineOffset {
    uint64_t lineOffset{0};
    uint64_t uncompressedOffsetInBytes{0};
  };

public:
  explicit ParallelGzipReader(UniqueFileReader fileReader,
                              size_t parallelization = 0,
                              uint64_t chunkSizeInBytes = 4_Mi)
      : m_chunkSizeInBytes(std::max(8_Ki, chunkSizeInBytes)),
        m_sharedFileReader(ensureSharedFileReader(std::move(fileReader))),
        m_fetcherParallelization(parallelization == 0 ? availableCores()
                                                      : parallelization),
        m_startBlockFinder([this]() {
          return std::make_unique<BlockFinder>(
              UniqueFileReader(m_sharedFileReader->clone()),
              m_chunkSizeInBytes);
        }) {
    setMaxDecompressedChunkSize(20U * m_chunkSizeInBytes);

    const auto fileSize = m_sharedFileReader->size();
    if (fileSize && (m_chunkSizeInBytes * 2U * parallelization > *fileSize)) {

      m_chunkSizeInBytes = std::max(
          512_Ki,
          ceilDiv(ceilDiv(*fileSize, 3U * parallelization), 512_Ki) * 512_Ki);
    }

    m_sharedFileReader->setStatisticsEnabled(m_statisticsEnabled);
    if (!m_sharedFileReader->seekable()) {

      throw std::logic_error("BitReader should always be seekable even if the "
                             "underlying file is not!");
    }

    const auto &[lock, file] = m_sharedFileReader->underlyingFile();

    auto *const singlePassFileReader =
        dynamic_cast<SinglePassFileReader *>(file);
    if (singlePassFileReader != nullptr) {
      singlePassFileReader->setMaxReusableChunkCount(static_cast<size_t>(
          std::ceil(static_cast<double>(parallelization) *
                    static_cast<double>(m_chunkSizeInBytes) /
                    static_cast<double>(SinglePassFileReader::CHUNK_SIZE))));
      setKeepIndex(false);
    }
  }

#ifdef WITH_PYTHON_SUPPORT

  explicit ParallelGzipReader(int fileDescriptor, size_t parallelization,
                              uint64_t chunkSizeInBytes,
                              IOReadMethod ioReadMethod)
      : ParallelGzipReader(
            wrapFileReader(std::make_unique<StandardFileReader>(fileDescriptor),
                           ioReadMethod),
            parallelization, chunkSizeInBytes) {}

  explicit ParallelGzipReader(const std::string &filePath,
                              size_t parallelization, uint64_t chunkSizeInBytes,
                              IOReadMethod ioReadMethod)
      : ParallelGzipReader(
            wrapFileReader(std::make_unique<StandardFileReader>(filePath),
                           ioReadMethod),
            parallelization, chunkSizeInBytes) {}

  explicit ParallelGzipReader(PyObject *pythonObject, size_t parallelization,
                              uint64_t chunkSizeInBytes,
                              IOReadMethod ioReadMethod)
      : ParallelGzipReader(
            wrapFileReader(std::make_unique<PythonFileReader>(pythonObject),
                           ioReadMethod),
            parallelization, chunkSizeInBytes) {}
#endif

  ~ParallelGzipReader() override {
    if (m_showProfileOnDestruction && m_statisticsEnabled) {
      std::stringstream out;
      out << std::boolalpha;
      out << "[ParallelGzipReader] Time spent:\n";
      out << "    Writing to output         : " << m_writeOutputTime << " s\n";
      out << "    Computing CRC32           : " << m_crc32Time << " s\n";
      out << "    Number of verified CRC32s : " << m_verifiedCRC32Count << "\n";
      out << "\nChunk Configuration:\n";
      out << "    CRC32 enabled      : " << m_crc32.enabled() << "\n";
      out << "    Window compression : "
          << (m_windowCompressionType ? toString(*m_windowCompressionType)
                                      : std::string("Default"))
          << "\n";
      out << "    Window sparsity    : " << m_windowSparsity << "\n";
      std::cerr << std::endl;
    }
  }

  void setStatisticsEnabled(bool enabled) {
    m_statisticsEnabled = enabled;
    if (m_chunkFetcher) {
      m_chunkFetcher->setStatisticsEnabled(m_statisticsEnabled);
    }
    if (m_sharedFileReader) {
      m_sharedFileReader->setStatisticsEnabled(m_statisticsEnabled);
    }
  }

  void setShowProfileOnDestruction(bool showProfileOnDestruction) {
    m_showProfileOnDestruction = showProfileOnDestruction;
    if (m_chunkFetcher) {
      m_chunkFetcher->setShowProfileOnDestruction(m_showProfileOnDestruction);
    }
    if (m_sharedFileReader) {
      m_sharedFileReader->setShowProfileOnDestruction(
          m_showProfileOnDestruction);
    }
  }

  [[nodiscard]] int fileno() const override {
    throw std::logic_error("This is a virtual file object, which has no "
                           "corresponding file descriptor!");
  }

  [[nodiscard]] bool seekable() const override {
    if (!m_sharedFileReader || !m_sharedFileReader->seekable()) {
      return false;
    }

    const auto &[lock, file] = m_sharedFileReader->underlyingFile();

    auto *const singlePassFileReader =
        dynamic_cast<SinglePassFileReader *>(file);
    return singlePassFileReader == nullptr;
  }

  void close() override {
    m_chunkFetcher.reset();
    m_blockFinder.reset();
    m_sharedFileReader.reset();
  }

  [[nodiscard]] bool closed() const override {
    return !m_sharedFileReader || m_sharedFileReader->closed();
  }

  [[nodiscard]] bool eof() const override { return m_atEndOfFile; }

  [[nodiscard]] bool fail() const override {
    throw std::logic_error("Not implemented!");
  }

  [[nodiscard]] size_t tell() const override {
    if (m_atEndOfFile) {
      const auto fileSize = size();
      if (!fileSize) {
        throw std::logic_error("When the file end has been reached, the block "
                               "map should have been finalized "
                               "and the file size should be available!");
      }
      return *fileSize;
    }
    return m_currentPosition;
  }

  [[nodiscard]] std::optional<size_t> size() const override {
    if (!m_blockMap->finalized()) {
      return std::nullopt;
    }
    return m_blockMap->back().second;
  }

  void clearerr() override {
    if (m_sharedFileReader) {
      m_sharedFileReader->clearerr();
    }
    m_atEndOfFile = false;
    throw std::invalid_argument("Not fully tested!");
  }

  [[nodiscard]] size_t read(char *outputBuffer, size_t nBytesToRead) override {
    return read(-1, outputBuffer, nBytesToRead);
  }

  size_t read(const int outputFileDescriptor = -1,
              char *const outputBuffer = nullptr,
              const size_t nBytesToRead = std::numeric_limits<size_t>::max()) {
    const auto writeFunctor =
        [nBytesDecoded = uint64_t(0), outputFileDescriptor, outputBuffer](
            const std::shared_ptr<ChunkData> &chunkData,
            size_t const offsetInBlock, size_t const dataToWriteSize) mutable {
          if (dataToWriteSize == 0) {
            return;
          }

          const auto errorCode = writeAll(chunkData, outputFileDescriptor,
                                          offsetInBlock, dataToWriteSize);
          if (errorCode != 0) {
            std::stringstream message;
            message << "Failed to write all bytes because of: "
                    << strerror(errorCode) << " (" << errorCode << ")";
            throw std::runtime_error(std::move(message).str());
          }

          if (outputBuffer != nullptr) {
            using rapidgzip::deflate::DecodedData;

            size_t nBytesCopied{0};
            for (auto it = DecodedData::Iterator(*chunkData, offsetInBlock,
                                                 dataToWriteSize);
                 static_cast<bool>(it); ++it) {
              const auto &[buffer, bufferSize] = *it;
              auto *const currentBufferPosition =
                  outputBuffer + nBytesDecoded + nBytesCopied;
              std::memcpy(currentBufferPosition, buffer, bufferSize);
              nBytesCopied += bufferSize;
            }
          }

          nBytesDecoded += dataToWriteSize;
        };

    if ((outputFileDescriptor == -1) && (outputBuffer == nullptr)) {

      return read(WriteFunctor{}, nBytesToRead);
    }
    return read(writeFunctor, nBytesToRead);
  }

  size_t read(const WriteFunctor &writeFunctor,
              const size_t nBytesToRead = std::numeric_limits<size_t>::max()) {
    if (!writeFunctor && m_blockMap->finalized()) {
      const auto oldOffset = tell();
      const auto newOffset =
          seek(nBytesToRead > static_cast<size_t>(
                                  std::numeric_limits<long long int>::max())
                   ? std::numeric_limits<long long int>::max()
                   : nBytesToRead,
               SEEK_CUR);
      return newOffset - oldOffset;
    }

    if (closed()) {
      throw std::invalid_argument(
          "You may not call read on closed ParallelGzipReader!");
    }

    if (eof() || (nBytesToRead == 0)) {
      return 0;
    }

    size_t nBytesDecoded = 0;
    while ((nBytesDecoded < nBytesToRead) && !eof()) {
#ifdef WITH_PYTHON_SUPPORT
      checkPythonSignalHandlers();
      const ScopedGILUnlock unlockedGIL;
#endif

      const auto blockResult = chunkFetcher().get(m_currentPosition);
      if (!blockResult) {
        m_atEndOfFile = true;
        break;
      }
      const auto &[decodedOffsetInBytes, chunkData] = *blockResult;

      if (chunkData->containsMarkers()) {
        throw std::logic_error("Did not expect to get results with markers!");
      }

      const auto offsetInBlock = m_currentPosition - decodedOffsetInBytes;
      const auto blockSize = chunkData->decodedSizeInBytes;
      if (offsetInBlock >= blockSize) {
        std::stringstream message;
        message << "[ParallelGzipReader] Block does not contain the requested "
                   "offset! "
                << "Requested offset from chunk fetcher: " << m_currentPosition
                << " (" << formatBytes(m_currentPosition) << ")"
                << ", decoded offset: " << decodedOffsetInBytes << " ("
                << formatBytes(decodedOffsetInBytes) << ")"
                << ", block data encoded offset: "
                << formatBits(chunkData->encodedOffsetInBits)
                << ", block data encoded size: "
                << formatBits(chunkData->encodedSizeInBits)
                << ", block data size: " << chunkData->decodedSizeInBytes
                << " (" << formatBytes(chunkData->decodedSizeInBytes) << ")"
                << " markers: " << chunkData->dataWithMarkersSize();
        throw std::logic_error(std::move(message).str());
      }

      const auto nBytesToDecode =
          std::min(blockSize - offsetInBlock, nBytesToRead - nBytesDecoded);

      [[maybe_unused]] const auto tCRC32Start = now();
      processCRC32(chunkData, offsetInBlock, nBytesToDecode);
      if (m_statisticsEnabled) {
        m_crc32Time += duration(tCRC32Start);
      }

      if (writeFunctor) {
        [[maybe_unused]] const auto tWriteStart = now();
        writeFunctor(chunkData, offsetInBlock, nBytesToDecode);
        if (m_statisticsEnabled) {
          m_writeOutputTime += duration(tWriteStart);
        }
      }

      nBytesDecoded += nBytesToDecode;
      m_currentPosition += nBytesToDecode;

      const auto &[lock, file] = m_sharedFileReader->underlyingFile();

      auto *const singlePassFileReader =
          dynamic_cast<SinglePassFileReader *>(file);
      if (singlePassFileReader != nullptr) {

        singlePassFileReader->releaseUpTo(chunkData->encodedOffsetInBits /
                                          CHAR_BIT);
      }

      if (!m_keepIndex && m_windowMap) {
        m_windowMap->releaseUpTo(chunkData->encodedOffsetInBits);
      }
    }

    return nBytesDecoded;
  }

  size_t seek(long long int offset, int origin = SEEK_SET) override {
    if (closed()) {
      throw std::invalid_argument(
          "You may not call seek on closed ParallelGzipReader!");
    }

    if (origin == SEEK_END) {

      if (!m_blockMap->finalized()) {
        read();
      }
    }
    const auto positiveOffset = effectiveOffset(offset, origin);

    if (positiveOffset == tell()) {

      m_atEndOfFile = m_blockMap->finalized() &&
                      (m_currentPosition >= m_blockMap->back().second);
      return positiveOffset;
    }

    if (positiveOffset < tell()) {
      if (!m_keepIndex) {
        throw std::invalid_argument("Seeking (back) not supported when "
                                    "index-keeping has been disabled!");
      }

      if (!seekable()) {
        throw std::invalid_argument(
            "Cannot seek backwards with non-seekable input!");
      }
      m_atEndOfFile = false;
      m_currentPosition = positiveOffset;
      return positiveOffset;
    }

    const auto blockInfo = m_blockMap->findDataOffset(positiveOffset);
    if (positiveOffset < blockInfo.decodedOffsetInBytes) {
      throw std::logic_error("Block map returned unwanted block!");
    }

    if (blockInfo.contains(positiveOffset)) {
      m_currentPosition = positiveOffset;
      m_atEndOfFile = m_blockMap->finalized() &&
                      (m_currentPosition >= m_blockMap->back().second);
      return tell();
    }

    if (m_blockMap->finalized()) {
      m_atEndOfFile = true;
      m_currentPosition = m_blockMap->back().second;
      return tell();
    }

    m_atEndOfFile = false;
    m_currentPosition =
        blockInfo.decodedOffsetInBytes + blockInfo.decodedSizeInBytes;
    read(-1, nullptr, positiveOffset - tell());
    return tell();
  }

  [[nodiscard]] bool blockOffsetsComplete() const {
    return m_blockMap->finalized();
  }

  [[nodiscard]] std::map<size_t, size_t> blockOffsets() {
    if (!m_blockMap->finalized()) {
      read();
      if (!m_blockMap->finalized() || !blockFinder().finalized()) {
        throw std::logic_error(
            "Reading everything should have finalized the block map!");
      }
    }

    return m_blockMap->blockOffsets();
  }

  [[nodiscard]] const GzipIndex gzipIndex(bool withLineOffsets = false) {
    const auto offsets = blockOffsets();
    if (offsets.empty() || !m_windowMap) {
      return {};
    }

    const auto archiveSize = m_sharedFileReader->size();
    if (!archiveSize && !m_indexIsImported) {

      std::cerr << "[Warning] The input file size should have become available "
                   "after finalizing the index!\n";
      std::cerr << "[Warning] Will use the last chunk end offset as size. This "
                   "might lead to errors on import!\n";
    }

    GzipIndex index;
    index.compressedSizeInBytes =
        archiveSize ? *archiveSize : ceilDiv(offsets.rbegin()->first, 8U);
    index.uncompressedSizeInBytes = offsets.rbegin()->second;
    index.windowSizeInBytes = 32_Ki;

    if (withLineOffsets) {
      if (!m_newlineCharacter) {
        throw std::runtime_error(
            "Cannot add line offsets to index when they were not gathered!");
      }
      index.hasLineOffsets = true;

      switch (m_newlineCharacter.value()) {
      case '\n':
        index.newlineFormat = NewlineFormat::LINE_FEED;
        break;
      case '\r':
        index.newlineFormat = NewlineFormat::CARRIAGE_RETURN;
        break;
      default:
        throw std::runtime_error("Cannot add line offsets to index when the "
                                 "gathered line offsets gathered are something "
                                 "other than newline or carriage return!");
      }
    }

    size_t maximumDecompressedSpacing{0};
    for (auto it = offsets.begin(), nit = std::next(offsets.begin());
         nit != offsets.end(); ++it, ++nit) {
      maximumDecompressedSpacing =
          std::max(maximumDecompressedSpacing, nit->second - it->second);
    }
    index.checkpointSpacing = maximumDecompressedSpacing / 32_Ki * 32_Ki;

    auto lineOffset = m_newlineOffsets.begin();
    for (const auto &[compressedOffsetInBits, uncompressedOffsetInBytes] :
         offsets) {
      Checkpoint checkpoint;
      checkpoint.compressedOffsetInBits = compressedOffsetInBits;
      checkpoint.uncompressedOffsetInBytes = uncompressedOffsetInBytes;

      if (index.hasLineOffsets) {
        while ((lineOffset != m_newlineOffsets.end()) &&
               (lineOffset->uncompressedOffsetInBytes <
                uncompressedOffsetInBytes)) {
          ++lineOffset;
        }

        if (lineOffset == m_newlineOffsets.end()) {
          std::stringstream message;
          message << "Failed to find line offset for uncompressed offset: "
                  << formatBytes(uncompressedOffsetInBytes)
                  << ", number of line offsets to stored: "
                  << m_newlineOffsets.size();
          throw std::runtime_error(std::move(message).str());
        }

        if (lineOffset->uncompressedOffsetInBytes !=
            uncompressedOffsetInBytes) {
          throw std::logic_error(
              "Line offset not found for uncompressed offset " +
              std::to_string(uncompressedOffsetInBytes) + "!");
        }

        checkpoint.lineOffset = lineOffset->lineOffset;
      }

      index.checkpoints.emplace_back(checkpoint);
    }

    index.windows = m_windowMap;

    return index;
  }

  [[nodiscard]] std::map<size_t, size_t> availableBlockOffsets() const {
    return m_blockMap->blockOffsets();
  }

  [[nodiscard]] auto statistics() const {
    if (!m_chunkFetcher) {
      throw std::invalid_argument("No chunk fetcher initialized!");
    }
    return m_chunkFetcher->statistics();
  }

  void setCRC32Enabled(bool enabled) {
    if (m_crc32.enabled() == enabled) {
      return;
    }

    m_crc32.setEnabled(enabled && (tell() == 0));
    applyChunkDataConfiguration();
  }

  void setMaxDecompressedChunkSize(uint64_t maxDecompressedChunkSize) {

    m_chunkConfiguration.maxDecompressedChunkSize =
        std::max(m_chunkSizeInBytes, maxDecompressedChunkSize);
    applyChunkDataConfiguration();
  }

  [[nodiscard]] uint64_t maxDecompressedChunkSize() const noexcept {
    return m_chunkConfiguration.maxDecompressedChunkSize;
  }

  void setNewlineCharacter(const std::optional<char> &newlineCharacter) {
    if (newlineCharacter == m_newlineCharacter) {
      return;
    }

    if (!m_newlineOffsets.empty() || (m_blockMap && !m_blockMap->empty())) {
      throw std::invalid_argument("May not change newline counting behavior "
                                  "after some chunks have been read!");
    }
    m_newlineCharacter = newlineCharacter;
    applyChunkDataConfiguration();
  }

  [[nodiscard]] std::optional<char> newlineCharacter() const noexcept {
    return m_newlineCharacter;
  }

  [[nodiscard]] const std::vector<NewlineOffset> &
  newlineOffsets() const noexcept {
    return m_newlineOffsets;
  }

private:
  void setBlockOffsets(const std::map<size_t, size_t> &offsets) {

    if (offsets.empty()) {
      if (m_blockMap->dataBlockCount() == 0) {
        return;
      }
      throw std::invalid_argument(
          "May not clear offsets. Construct a new ParallelGzipReader instead!");
    }

    setBlockFinderOffsets(offsets);

    if (offsets.size() < 2) {
      throw std::invalid_argument("Block offset map must contain at least one "
                                  "valid block and one EOS block!");
    }
    m_blockMap->setBlockOffsets(offsets);
  }

public:
  void setBlockOffsets(const GzipIndex &index) {
    if (index.checkpoints.empty() || !index.windows) {
      return;
    }

    const auto lockedWindows = index.windows->data();
    if (lockedWindows.second == nullptr) {
      throw std::invalid_argument("Index window map must be a valid pointer!");
    }

    const auto lessOffset = [](const auto &a, const auto &b) {
      return a.uncompressedOffsetInBytes < b.uncompressedOffsetInBytes;
    };
    if (!std::is_sorted(index.checkpoints.begin(), index.checkpoints.end(),
                        lessOffset)) {
      throw std::invalid_argument(
          "Index checkpoints must be sorted by uncompressed offsets!");
    }

    m_indexIsImported = true;
    m_keepIndex = true;

    std::optional<char> newlineCharacter;
    switch (index.newlineFormat) {
    case NewlineFormat::LINE_FEED:
      newlineCharacter = '\n';
      break;
    case NewlineFormat::CARRIAGE_RETURN:
      newlineCharacter = '\r';
      break;
    }

    if (index.hasLineOffsets && newlineCharacter) {
      m_newlineCharacter = newlineCharacter;
      m_newlineOffsets.resize(index.checkpoints.size());
      std::transform(index.checkpoints.begin(), index.checkpoints.end(),
                     m_newlineOffsets.begin(), [](const auto &checkpoint) {
                       NewlineOffset newlineOffset;
                       newlineOffset.lineOffset = checkpoint.lineOffset;
                       newlineOffset.uncompressedOffsetInBytes =
                           checkpoint.uncompressedOffsetInBytes;
                       return newlineOffset;
                     });

      const auto lessLineOffset = [](const auto &a, const auto &b) {
        return a.lineOffset < b.lineOffset;
      };
      if (!std::is_sorted(m_newlineOffsets.begin(), m_newlineOffsets.end(),
                          lessLineOffset)) {
        throw std::invalid_argument(
            "Index checkpoints must be sorted by line offsets!");
      }
    }

    std::map<size_t, size_t> newBlockOffsets;
    for (size_t i = 0; i < index.checkpoints.size(); ++i) {
      const auto &checkpoint = index.checkpoints[i];

      if (!newBlockOffsets.empty() && (i + 1 < index.checkpoints.size()) &&
          (index.checkpoints[i + 1].uncompressedOffsetInBytes -
               newBlockOffsets.rbegin()->second <=
           m_chunkSizeInBytes)) {
        continue;
      }

      newBlockOffsets.emplace(checkpoint.compressedOffsetInBits,
                              checkpoint.uncompressedOffsetInBytes);

      if (const auto window =
              lockedWindows.second->find(checkpoint.compressedOffsetInBits);
          window != lockedWindows.second->end()) {
        m_windowMap->emplaceShared(checkpoint.compressedOffsetInBits,
                                   window->second);
      }
    }

    if (const auto fileEndOffset =
            newBlockOffsets.find(index.compressedSizeInBytes * 8);
        fileEndOffset == newBlockOffsets.end()) {
      newBlockOffsets.emplace(index.compressedSizeInBytes * 8,
                              index.uncompressedSizeInBytes);
      m_windowMap->emplace(index.compressedSizeInBytes * 8, {},
                           CompressionType::NONE);
    } else if (fileEndOffset->second != index.uncompressedSizeInBytes) {
      throw std::invalid_argument(
          "Index has contradicting information for the file end information!");
    }

    setBlockOffsets(newBlockOffsets);

    chunkFetcher().clearCache();
  }

  void importIndex(UniqueFileReader indexFile) {
    const auto t0 = now();
    setBlockOffsets(readGzipIndex(std::move(indexFile),
                                  m_sharedFileReader->clone(),
                                  m_fetcherParallelization));
    if (m_showProfileOnDestruction) {
      std::cerr << "[ParallelGzipReader::importIndex] Took " << duration(t0)
                << " s\n";
    }
  }

  void exportIndex(
      const std::function<void(const void *buffer, size_t size)> &checkedWrite,
      const IndexFormat indexFormat = IndexFormat::INDEXED_GZIP) {
    const auto t0 = now();

    if (!m_keepIndex) {
      throw std::invalid_argument("Exporting index not supported when "
                                  "index-keeping has been disabled!");
    }

    switch (indexFormat) {
    case IndexFormat::INDEXED_GZIP:
      indexed_gzip::writeGzipIndex(gzipIndex(), checkedWrite);
      break;
    case IndexFormat::GZTOOL:
      gztool::writeGzipIndex(gzipIndex(false), checkedWrite);
      break;
    case IndexFormat::GZTOOL_WITH_LINES:
      gztool::writeGzipIndex(gzipIndex(true), checkedWrite);
      break;
    }

    if (m_showProfileOnDestruction) {
      std::cerr << "[ParallelGzipReader::exportIndex] Took " << duration(t0)
                << " s\n";
    }
  }

#ifdef WITH_PYTHON_SUPPORT
  void importIndex(PyObject *pythonObject) {
    importIndex(std::make_unique<PythonFileReader>(pythonObject));
  }

  void exportIndex(PyObject *pythonObject,
                   const IndexFormat indexFormat = IndexFormat::INDEXED_GZIP) {
    const auto file = std::make_unique<PythonFileReader>(pythonObject);
    const auto checkedWrite = [&file](const void *buffer, size_t size) {
      if (file->write(reinterpret_cast<const char *>(buffer), size) != size) {
        throw std::runtime_error("Failed to write data to index!");
      }
    };

    exportIndex(checkedWrite, indexFormat);
  }
#endif

  void gatherLineOffsets() {

    if (!m_newlineCharacter) {
      return;
    }

    const Finally restorePosition{
        [this, oldOffset = tell()]() { seek(oldOffset); }};

    if (!blockOffsetsComplete()) {
      read();
      return;
    }

    uint64_t processedBytes{
        m_newlineOffsets.empty()
            ? 0
            : m_newlineOffsets.back().uncompressedOffsetInBytes};
    if (const auto fileSize = size();
        fileSize && !m_newlineOffsets.empty() && (processedBytes >= fileSize)) {
      return;
    }

    uint64_t processedLines{
        m_newlineOffsets.empty() ? 0 : m_newlineOffsets.back().lineOffset};

    std::vector<uint64_t> newlineOffsets;

    const auto collectLineOffsets =
        [this, &processedLines, &newlineOffsets, &processedBytes,
         newlineCharacter = m_newlineCharacter.value()](
            const std::shared_ptr<rapidgzip::ChunkData> &chunkData,
            const size_t offsetInChunk, const size_t dataToWriteSize) {
          using rapidgzip::deflate::DecodedData;
          for (auto it = DecodedData::Iterator(*chunkData, offsetInChunk,
                                               dataToWriteSize);
               static_cast<bool>(it); ++it) {
            const auto &[buffer, size] = *it;

            const std::string_view view{reinterpret_cast<const char *>(buffer),
                                        size};
            for (auto position = view.find(newlineCharacter, 0);
                 position != std::string_view::npos;
                 position = view.find(newlineCharacter, position + 1)) {
              newlineOffsets.emplace_back(processedBytes + position);
            }

            processedBytes += size;
          }

          auto it = newlineOffsets.begin();
          while (it != newlineOffsets.end()) {
            const auto chunkInfo = m_blockMap->findDataOffset(*it);
            if (!chunkInfo.contains(*it)) {

              std::cerr << "[Warning] Offset in processed chunk was not found "
                           "in chunk map!\n";
              break;
            }

            if (m_newlineOffsets.empty() ||
                (m_newlineOffsets.back().uncompressedOffsetInBytes != *it)) {
              NewlineOffset newlineOffset;
              newlineOffset.lineOffset = static_cast<uint64_t>(std::distance(
                                             newlineOffsets.begin(), it)) +
                                         processedLines;
              newlineOffset.uncompressedOffsetInBytes =
                  chunkInfo.decodedOffsetInBytes;

              if (!m_newlineOffsets.empty()) {
                if (m_newlineOffsets.back().uncompressedOffsetInBytes >= *it) {
                  std::stringstream message;
                  message << "Got earlier or equal chunk offset than the last "
                             "processed one! "
                          << "Last newline byte offset: "
                          << m_newlineOffsets.back().uncompressedOffsetInBytes
                          << ", found newline byte offset: " << *it;
                  throw std::logic_error(std::move(message).str());
                }

                if (m_newlineOffsets.back().lineOffset >
                    newlineOffset.lineOffset) {
                  throw std::logic_error(
                      "Got earlier line offset than the last processed one!");
                }
              }

              m_newlineOffsets.emplace_back(newlineOffset);
            }

            while ((it != newlineOffsets.end()) && chunkInfo.contains(*it)) {
              ++it;
            }
          }

          processedLines +=
              static_cast<uint64_t>(std::distance(newlineOffsets.begin(), it));
          newlineOffsets.erase(newlineOffsets.begin(), it);
        };

    seekTo(processedBytes);
    read(collectLineOffsets);

    if (m_newlineOffsets.empty() ||
        (processedBytes > m_newlineOffsets.back().uncompressedOffsetInBytes)) {
      NewlineOffset newlineOffset;
      newlineOffset.uncompressedOffsetInBytes = processedBytes;
      newlineOffset.lineOffset = processedLines + newlineOffsets.size();
      m_newlineOffsets.emplace_back(newlineOffset);
    }
  }

  [[nodiscard]] size_t tellCompressed() const {
    if (!m_blockMap || m_blockMap->empty()) {
      return 0;
    }

    const auto blockInfo = m_blockMap->findDataOffset(m_currentPosition);
    if (blockInfo.contains(m_currentPosition)) {
      return blockInfo.encodedOffsetInBits;
    }
    return m_blockMap->back().first;
  }

  void joinThreads() {
    m_chunkFetcher.reset();
    m_blockFinder.reset();
  }

  void setKeepIndex(bool keep) {
    m_keepIndex = keep;
    applyChunkDataConfiguration();
  }

  void setWindowSparsity(bool useSparseWindows) {
    m_windowSparsity = useSparseWindows;
    applyChunkDataConfiguration();
  }

  void setWindowCompressionType(CompressionType windowCompressionType) {
    m_windowCompressionType = windowCompressionType;
    applyChunkDataConfiguration();
  }

  [[nodiscard]] std::string fileTypeAsString() {
    return toString(blockFinder().fileType());
  }

  void setDeflateStreamCRC32s(std::unordered_map<size_t, uint32_t> crc32s) {
    m_deflateStreamCRC32s = std::move(crc32s);
  }

  void addDeflateStreamCRC32(size_t endOfStreamOffsetInBytes, uint32_t crc32) {
    m_deflateStreamCRC32s.insert_or_assign(endOfStreamOffsetInBytes, crc32);
  }

private:
  void applyChunkDataConfiguration() {
    if (!m_chunkFetcher) {
      return;
    }

    m_chunkConfiguration.crc32Enabled = m_crc32.enabled();
    m_chunkConfiguration.windowCompressionType =
        m_keepIndex ? m_windowCompressionType
                    : std::make_optional(CompressionType::NONE);

    m_chunkConfiguration.windowSparsity = m_keepIndex && m_windowSparsity;
    m_chunkConfiguration.newlineCharacter = m_newlineCharacter;

    m_chunkFetcher->setChunkConfiguration(m_chunkConfiguration);
  }

  BlockFinder &blockFinder() {

    if (m_blockFinder) {
      return *m_blockFinder;
    }

    if (!m_startBlockFinder) {
      throw std::logic_error(
          "Block finder creator was not initialized correctly!");
    }

    m_blockFinder = m_startBlockFinder();
    if (!m_blockFinder) {
      throw std::logic_error(
          "Block finder creator failed to create new block finder!");
    }

    if (m_blockMap->finalized()) {
      setBlockFinderOffsets(m_blockMap->blockOffsets());
    }

    return *m_blockFinder;
  }

  ChunkFetcher &chunkFetcher() {
    if (m_chunkFetcher) {
      return *m_chunkFetcher;
    }

    blockFinder();

    m_chunkFetcher = std::make_unique<ChunkFetcher>(
        ensureSharedFileReader(m_sharedFileReader->clone()), m_blockFinder,
        m_blockMap, m_windowMap, m_fetcherParallelization);
    if (!m_chunkFetcher) {
      throw std::logic_error("Block fetcher should have been initialized!");
    }

    m_chunkFetcher->setShowProfileOnDestruction(m_showProfileOnDestruction);
    m_chunkFetcher->setStatisticsEnabled(m_statisticsEnabled);
    m_chunkFetcher->addChunkIndexingCallback(
        [this](const auto &chunk, auto) { this->gatherLineOffsets(chunk); });
    applyChunkDataConfiguration();

    return *m_chunkFetcher;
  }

  void gatherLineOffsets(const std::shared_ptr<const ChunkData> &chunk) {
    if (!m_newlineCharacter.has_value()) {
      return;
    }

    if (!chunk) {
      throw std::logic_error("ParallelGzipReader::gatherLineOffsets should "
                             "only be called with valid chunk!");
    }

    for (const auto &subchunk : chunk->subchunks()) {
      if (!subchunk.newlineCount) {
        throw std::logic_error("Newline count in subchunk is missing!");
      }
      if (chunk->configuration.newlineCharacter != m_newlineCharacter) {
        throw std::logic_error(
            "Newline character in subchunk does not match the configured one!");
      }

      const auto blockInfo =
          m_blockMap->getEncodedOffset(subchunk.encodedOffset);
      if (!blockInfo) {
        std::stringstream message;
        message << "Failed to find subchunk offset: "
                << formatBits(subchunk.encodedOffset)
                << "even though it should have been inserted at the top of "
                   "this method!";
        throw std::logic_error(std::move(message).str());
      }

      if (m_newlineOffsets.empty()) {
        m_newlineOffsets.emplace_back(NewlineOffset{0, 0});
      }

      const auto &lastLineCount = m_newlineOffsets.back();
      if (lastLineCount.uncompressedOffsetInBytes !=
          blockInfo->decodedOffsetInBytes) {
        std::stringstream message;
        message << "Did not find line count for preceding decompressed offset: "
                << formatBytes(blockInfo->decodedOffsetInBytes);
        throw std::logic_error(std::move(message).str());
      }

      m_newlineOffsets.emplace_back(NewlineOffset{
          lastLineCount.lineOffset + subchunk.newlineCount.value(),
          blockInfo->decodedOffsetInBytes + subchunk.decodedSize});
    }
  }

  void setBlockFinderOffsets(const std::map<size_t, size_t> &offsets) {
    if (offsets.empty()) {
      throw std::invalid_argument(
          "A non-empty list of block offsets is required!");
    }

    typename BlockFinder::BlockOffsets encodedBlockOffsets;
    for (auto it = offsets.begin(), nit = std::next(offsets.begin());
         nit != offsets.end(); ++it, ++nit) {

      if (it->second != nit->second) {
        encodedBlockOffsets.push_back(it->first);
      }
    }

    blockFinder().setBlockOffsets(std::move(encodedBlockOffsets));
  }

  void processCRC32(const std::shared_ptr<ChunkData> &chunkData,
                    [[maybe_unused]] size_t const offsetInBlock,
                    [[maybe_unused]] size_t const dataToWriteSize) {
    if ((m_nextCRC32ChunkOffset == 0) && m_blockFinder) {
      const auto [offset, errorCode] = m_blockFinder->get(0, 0);
      if (offset && (errorCode == BlockFinder::GetReturnCode::SUCCESS)) {
        m_nextCRC32ChunkOffset = *offset;
      }
    }

    if (!m_crc32.enabled() ||
        (m_nextCRC32ChunkOffset != chunkData->encodedOffsetInBits) ||
        chunkData->crc32s.empty()) {
      return;
    }

    m_nextCRC32ChunkOffset =
        chunkData->encodedOffsetInBits + chunkData->encodedSizeInBits;

    if (chunkData->crc32s.size() != chunkData->footers.size() + 1) {
      throw std::logic_error(
          "Fewer CRC32s in chunk than expected based on the gzip footers!");
    }

    const auto totalCRC32StreamSize =
        std::accumulate(chunkData->crc32s.begin(), chunkData->crc32s.end(),
                        size_t(0), [](size_t sum, const auto &calculator) {
                          return sum + calculator.streamSize();
                        });
    if (totalCRC32StreamSize != chunkData->decodedSizeInBytes) {
      std::stringstream message;
      message << "CRC32 computation stream size ("
              << formatBytes(totalCRC32StreamSize) << ") differs from "
              << "chunk size: " << formatBytes(chunkData->decodedSizeInBytes)
              << "!\n"
              << "Please open an issue or disable integrated CRC32 "
                 "verification as a quick workaround.";
      throw std::logic_error(std::move(message).str());
    }

    m_crc32.append(chunkData->crc32s.front());
    for (size_t i = 0; i < chunkData->footers.size(); ++i) {
      const auto &footer = chunkData->footers[i];
      const auto footerByteOffset =
          ceilDiv(footer.blockBoundary.encodedOffset, CHAR_BIT);
      if (const auto externalCRC32 =
              m_deflateStreamCRC32s.find(footerByteOffset);
          externalCRC32 != m_deflateStreamCRC32s.end()) {
        m_crc32.verify(m_crc32.crc32());
      } else if (hasCRC32(chunkData->configuration.fileType) &&
                 m_crc32.verify(footer.gzipFooter.crc32)) {
        m_verifiedCRC32Count++;
      }
      m_crc32 = chunkData->crc32s.at(i + 1);
    }
  }

private:
  uint64_t m_chunkSizeInBytes{4_Mi};
  ChunkConfiguration m_chunkConfiguration;

  std::unique_ptr<SharedFileReader> m_sharedFileReader;

  size_t m_currentPosition = 0;

  bool m_atEndOfFile = false;

  bool m_statisticsEnabled{false};
  bool m_showProfileOnDestruction{false};
  double m_writeOutputTime{0};
  double m_crc32Time{0};
  uint64_t m_verifiedCRC32Count{0};

  size_t const m_fetcherParallelization;

  std::function<std::shared_ptr<BlockFinder>(void)> const m_startBlockFinder;

  std::shared_ptr<BlockFinder> m_blockFinder;
  std::shared_ptr<BlockMap> const m_blockMap{std::make_shared<BlockMap>()};

  std::shared_ptr<WindowMap> const m_windowMap{std::make_shared<WindowMap>()};
  bool m_keepIndex{true};
  bool m_windowSparsity{true};
  std::optional<CompressionType> m_windowCompressionType;
  std::unique_ptr<ChunkFetcher> m_chunkFetcher;

  std::vector<NewlineOffset> m_newlineOffsets;
  std::optional<char> m_newlineCharacter;

  CRC32Calculator m_crc32;
  uint64_t m_nextCRC32ChunkOffset{0};
  std::unordered_map<size_t, uint32_t> m_deflateStreamCRC32s;

  bool m_indexIsImported{false};
};
} // namespace rapidgzip
