#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <FasterVector.hpp>
#include <JoiningThread.hpp>
#include <common.hpp>

#include "FileReader.hpp"

namespace rapidgzip {

class SinglePassFileReader final : public FileReader {
public:
  static constexpr size_t CHUNK_SIZE = 4_Mi;

private:
  using Chunk = FasterVector<std::byte>;

public:
  explicit SinglePassFileReader(UniqueFileReader fileReader)
      : m_file(std::move(fileReader)),
        m_fileno(m_file ? m_file->fileno() : -1) {}

  ~SinglePassFileReader() override { close(); }

  [[nodiscard]] UniqueFileReader cloneRaw() const override {
    throw std::invalid_argument(
        "Cloning file reader not allowed because the internal file position "
        "should not be modified by multiple owners!");
  }

  void close() override {
    m_cancelReaderThread = true;
    m_notifyReaderThread.notify_one();
    m_readerThread.reset();

    if (m_file) {
      m_file->close();
    }
  }

  [[nodiscard]] bool closed() const override {
    return !m_file || m_file->closed();
  }

  [[nodiscard]] bool eof() const override {
    return m_underlyingFileEOF && (m_currentPosition >= m_numberOfBytesRead);
  }

  [[nodiscard]] bool fail() const override { return false; }

  [[nodiscard]] int fileno() const override {
    if (m_file) {
      return m_fileno;
    }
    throw std::invalid_argument("Trying to get fileno of an invalid file!");
  }

  [[nodiscard]] bool seekable() const override { return true; }

  [[nodiscard]] size_t read(char *buffer, size_t nMaxBytesToRead) override {
    if (nMaxBytesToRead == 0) {
      return 0;
    }

    bufferUpTo(saturatingAddition(m_currentPosition, nMaxBytesToRead));
    const std::lock_guard lock(m_bufferMutex);
    const auto startChunk = getChunkIndexUnsafe(m_currentPosition);

    size_t nBytesRead{0};
    for (size_t i = startChunk;
         (i < m_buffer.size()) && (nBytesRead < nMaxBytesToRead); ++i) {
      const auto chunkOffset = i * CHUNK_SIZE;
      const auto &chunk = getChunk(i);
      const auto *sourceOffset = chunk.data();
      auto nAvailableBytes = chunk.size();

      if (chunkOffset < m_currentPosition) {
        if (m_currentPosition - chunkOffset > nAvailableBytes) {
          throw std::logic_error(
              "Calculation of start chunk seems to be wrong!");
        }

        const auto nBytesToSkip = m_currentPosition - chunkOffset;
        nAvailableBytes -= nBytesToSkip;
        sourceOffset += nBytesToSkip;
      }

      const auto nBytesToCopy =
          std::min(nAvailableBytes, nMaxBytesToRead - nBytesRead);
      if (buffer != nullptr) {
        std::memcpy(buffer + nBytesRead, sourceOffset, nBytesToCopy);
      }
      nBytesRead += nBytesToCopy;
    }

    m_currentPosition += nBytesRead;

    return nBytesRead;
  }

  size_t seek(long long int offset, int origin = SEEK_SET) override {
    if (origin == SEEK_END) {
      bufferUpTo(std::numeric_limits<size_t>::max());
    }
    m_currentPosition = effectiveOffset(offset, origin);
    return m_currentPosition;
  }

  [[nodiscard]] std::optional<size_t> size() const override {
    if (m_underlyingFileEOF) {
      return m_numberOfBytesRead;
    }
    if (m_file) {
      const auto underlyingSize = m_file->size();

      if (underlyingSize && (m_file->seekable() || (*underlyingSize > 0))) {
        return underlyingSize;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] size_t tell() const override { return m_currentPosition; }

  void clearerr() override {}

  void releaseUpTo(const size_t untilOffset) {
    const std::scoped_lock lock(m_bufferMutex);

    if (m_buffer.size() <= 1) {
      return;
    }

    const auto lastChunkToRelease =
        std::min(untilOffset / CHUNK_SIZE, m_buffer.size() - 2);
    for (auto i = m_releasedChunkCount; i < lastChunkToRelease; ++i) {
      if (m_reusableChunks.size() < m_maxReusableChunkCount) {
        std::swap(m_buffer[i], m_reusableChunks.emplace_back());
      } else {
        m_buffer[i] = Chunk();
      }
    }
    m_releasedChunkCount = lastChunkToRelease;
  }

  [[nodiscard]] size_t setMaxReusableChunkCount() const noexcept {
    return m_maxReusableChunkCount;
  }

  void setMaxReusableChunkCount(const size_t maxReusableChunkCount) {
    m_maxReusableChunkCount = maxReusableChunkCount;
    if (m_reusableChunks.size() > m_maxReusableChunkCount) {
      m_reusableChunks.resize(m_maxReusableChunkCount);
    }
  }

private:
  void bufferUpTo(const size_t untilOffset) {
    if (m_underlyingFileEOF || (untilOffset <= m_bufferUntilOffset)) {
      return;
    }

    m_bufferUntilOffset = untilOffset;
    m_notifyReaderThread.notify_one();

    std::unique_lock lock(m_bufferMutex);
    m_bufferChanged.wait(lock, [this, untilOffset]() {
      return m_underlyingFileEOF ||
             (m_buffer.size() * CHUNK_SIZE >= untilOffset);
    });
  }

  void readerThreadMain() {

    if (!m_file) {
      return;
    }

    while (!m_cancelReaderThread && !m_underlyingFileEOF) {
      if (m_numberOfBytesRead >=
          saturatingAddition(m_bufferUntilOffset.load(), 64 * CHUNK_SIZE)) {
        std::unique_lock lock(m_bufferUntilOffsetMutex);
        m_notifyReaderThread.wait(lock, [this]() {
          return m_cancelReaderThread ||
                 (m_numberOfBytesRead <
                  saturatingAddition(m_bufferUntilOffset.load(),
                                     64 * CHUNK_SIZE));
        });
        continue;
      }

      Chunk chunk;
      {
        const std::lock_guard lock(m_bufferMutex);
        if (!m_reusableChunks.empty()) {
          std::swap(chunk, m_reusableChunks.back());
          m_reusableChunks.pop_back();
        }
      }

      chunk.resize(CHUNK_SIZE);

      size_t nBytesBuffered{0};
      while (nBytesBuffered < chunk.size()) {
        const auto nBytesRead = m_file->read(
            reinterpret_cast<char *>(chunk.data()) + nBytesBuffered,
            chunk.size() - nBytesBuffered);
        if (nBytesRead == 0) {
          break;
        }
        nBytesBuffered += nBytesRead;
      }
      chunk.resize(nBytesBuffered);

      {
        const std::lock_guard lock(m_bufferMutex);
        m_numberOfBytesRead += nBytesBuffered;
        m_underlyingFileEOF = nBytesBuffered < CHUNK_SIZE;
        m_buffer.emplace_back(std::move(chunk));
      }
      m_bufferChanged.notify_all();
    }
  }

  [[nodiscard]] size_t getChunkIndexUnsafe(const size_t offset) const {

    const auto startChunk = offset / CHUNK_SIZE;
    if (offset < m_numberOfBytesRead) {
      if (startChunk >= m_buffer.size()) {
        throw std::logic_error("[SinglePassFileReader] Current position is "
                               "inside file but failed to find "
                               "chunk!");
      }
      if (m_buffer[startChunk].empty()) {
        std::stringstream message;
        message << "[SinglePassFileReader] Trying to access chunk "
                << startChunk << " out of " << m_buffer.size() << " at offset "
                << formatBits(offset)
                << ", which has already been released! Released chunk count: "
                << m_releasedChunkCount << "\n";
        throw std::invalid_argument(std::move(message).str());
      }
    }

    return startChunk;
  }

  [[nodiscard]] const Chunk &getChunk(size_t index) const {
    const auto &chunk = m_buffer.at(index);

    if ((index + 1 < m_buffer.size()) && (chunk.size() != CHUNK_SIZE)) {
      std::stringstream message;
      message << "[SinglePassFileReader] All but the last chunk must be of "
                 "equal size! Chunk "
              << index << " out of " << m_buffer.size() << " has size "
              << formatBytes(chunk.size()) << " instead of expected "
              << formatBytes(CHUNK_SIZE) << "!";
      throw std::logic_error(std::move(message).str());
    }

    return chunk;
  }

protected:
  const UniqueFileReader m_file;
  const int m_fileno;

  size_t m_currentPosition{0};

  std::atomic<size_t> m_bufferUntilOffset{0};
  mutable std::mutex m_bufferUntilOffsetMutex;

  std::atomic<bool> m_underlyingFileEOF{false};
  std::atomic<size_t> m_numberOfBytesRead{0};

  size_t m_releasedChunkCount{0};
  std::deque<Chunk> m_buffer;
  mutable std::mutex m_bufferMutex;
  std::condition_variable m_bufferChanged;

  size_t m_maxReusableChunkCount{1};
  std::deque<Chunk> m_reusableChunks;

  std::atomic<bool> m_cancelReaderThread{false};

  std::condition_variable m_notifyReaderThread;

  std::unique_ptr<JoiningThread> m_readerThread{
      std::make_unique<JoiningThread>([this]() { readerThreadMain(); })};
};

} // namespace rapidgzip
