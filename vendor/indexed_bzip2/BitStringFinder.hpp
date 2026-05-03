#pragma once

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <type_traits>
#include <utility>
#include <vector>

#include <BitManipulation.hpp>
#include <FileReader.hpp>
#include <common.hpp>

namespace rapidgzip {

template <uint8_t bitStringSize> class BitStringFinder {
public:
  using ShiftedLUTTable = std::vector<std::pair<uint64_t, uint64_t>>;

public:
  BitStringFinder(BitStringFinder &&) noexcept = default;

  BitStringFinder(const BitStringFinder &other) = delete;

  BitStringFinder &operator=(const BitStringFinder &) = delete;

  BitStringFinder &operator=(BitStringFinder &&) noexcept = delete;

  BitStringFinder(UniqueFileReader fileReader, uint64_t bitStringToFind,
                  size_t fileBufferSizeBytes = 1_Mi)
      : m_bitStringToFind(bitStringToFind &
                          nLowestBitsSet<uint64_t>(bitStringSize)),
        m_movingBitsToKeep(bitStringSize > 0 ? bitStringSize - 1U : 0U),
        m_movingBytesToKeep(ceilDiv(m_movingBitsToKeep, CHAR_BIT)),
        m_fileReader(std::move(fileReader)),
        m_fileChunksInBytes(
            std::max(fileBufferSizeBytes,
                     static_cast<size_t>(ceilDiv(bitStringSize, CHAR_BIT)))) {
    if (m_movingBytesToKeep >= m_fileChunksInBytes) {
      std::stringstream msg;
      msg << "The file buffer size of " << m_fileChunksInBytes
          << "B is too small to look for strings with " << bitStringSize
          << " bits!";
      throw std::invalid_argument(std::move(msg).str());
    }
  }

  BitStringFinder(const char *buffer, size_t size, uint64_t bitStringToFind)
      : BitStringFinder(UniqueFileReader(), bitStringToFind) {
    m_buffer.assign(buffer, buffer + size);
  }

  virtual ~BitStringFinder() = default;

  [[nodiscard]] bool seekable() const {

    return !m_fileReader || m_fileReader->seekable();
  }

  [[nodiscard]] bool eof() const {
    if (m_fileReader) {
      return bufferEof() && m_fileReader->eof();
    }
    return m_buffer.empty();
  }

  [[nodiscard]] virtual size_t find();

protected:
  [[nodiscard]] bool bufferEof() const {
    return m_bufferBitsRead >= m_buffer.size() * CHAR_BIT;
  }

  size_t refillBuffer();

public:
  [[nodiscard]] static std::vector<size_t>
  findBitStrings(std::string_view const &buffer, uint64_t bitString);

protected:
  const uint64_t m_bitStringToFind;

  const uint8_t m_movingBitsToKeep;
  const uint8_t m_movingBytesToKeep;

  std::vector<char> m_buffer;
  std::vector<size_t> m_offsetsInBuffer;

  size_t m_bufferBitsRead = 0;

  UniqueFileReader m_fileReader;

  const size_t m_fileChunksInBytes;

  size_t m_nTotalBytesRead = 0;
};

template <uint8_t bitStringSize>
std::vector<size_t>
BitStringFinder<bitStringSize>::findBitStrings(std::string_view const &buffer,
                                               uint64_t bitString) {
  static_assert(
      (bitStringSize >= CHAR_BIT) && (bitStringSize % CHAR_BIT == 0),
      "This is a highly optimized bit string finder for bzip2 magic bytes.");

  const auto findStrings = [](std::string_view const &data,
                              std::string_view const &stringToFind) {
    std::vector<size_t> blockOffsets;
    for (auto position = data.find(stringToFind, 0);
         position != std::string_view::npos;
         position = data.find(stringToFind, position + 1U)) {
      blockOffsets.push_back(position);
    }
    return blockOffsets;
  };

  const auto msbToString = [](uint64_t const bitStringToConvert,
                              uint8_t const bitStringtoConvertSize) {
    std::vector<char> result(bitStringtoConvertSize / CHAR_BIT);
    auto remainingSize = bitStringtoConvertSize;
    for (auto &value : result) {
      remainingSize -= CHAR_BIT;
      value = static_cast<char>((bitStringToConvert >> remainingSize) & 0xFFU);
    }
    return result;
  };

  const auto headMatches = [&](size_t offset, uint32_t nBitsBefore) {
    if (nBitsBefore == 0) {
      return true;
    }
    if ((offset == 0) || ((offset - 1) >= buffer.size())) {
      return false;
    }
    return (static_cast<uint8_t>(buffer[offset - 1]) &
            nLowestBitsSet<uint64_t>(nBitsBefore)) ==
           ((bitString >> (bitStringSize - nBitsBefore)) &
            nLowestBitsSet<uint64_t>(nBitsBefore));
  };

  const auto tailMatches = [&](size_t offset, uint32_t nBitsAfter) {
    if (nBitsAfter == 0) {
      return true;
    }
    if (offset >= buffer.size()) {
      return false;
    }
    return (static_cast<uint64_t>(static_cast<uint8_t>(buffer[offset])) >>
            (CHAR_BIT - nBitsAfter)) ==
           (bitString & nLowestBitsSet<uint64_t>(nBitsAfter));
  };

  std::vector<size_t> blockOffsets;
  for (uint32_t shift = 0U; shift < CHAR_BIT; ++shift) {
    const auto stringToFind =
        msbToString(bitString >> shift, bitStringSize - CHAR_BIT);
    const auto newBlockOffsets =
        findStrings(buffer, {stringToFind.data(), stringToFind.size()});

    const auto nBitsAfter = shift;
    const auto nBitsBefore = CHAR_BIT - shift;

    assert(stringToFind.size() == bitStringSize / CHAR_BIT - 1U);
    for (const auto offset : newBlockOffsets) {
      if (headMatches(offset, nBitsBefore) &&
          tailMatches(offset + stringToFind.size(), nBitsAfter)) {
        blockOffsets.push_back(offset * CHAR_BIT - nBitsBefore);
      }
    }
  }

  return blockOffsets;
}

template <uint8_t bitStringSize> size_t BitStringFinder<bitStringSize>::find() {
  if (bitStringSize == 0) {
    return std::numeric_limits<size_t>::max();
  }

  if (!m_offsetsInBuffer.empty()) {
    const auto offset = m_nTotalBytesRead * CHAR_BIT + m_offsetsInBuffer.back();
    m_offsetsInBuffer.pop_back();
    return offset;
  }

  while (!eof()) {
    if (bufferEof()) {
      const auto nBytesRead = refillBuffer();
      if (nBytesRead == 0) {
        return std::numeric_limits<size_t>::max();
      }
    }

    m_offsetsInBuffer =
        findBitStrings({m_buffer.data(), m_buffer.size()}, m_bitStringToFind);

    const auto firstBitsToIgnore =
        static_cast<uint8_t>(m_bufferBitsRead % CHAR_BIT);
    m_offsetsInBuffer.erase(std::remove_if(m_offsetsInBuffer.begin(),
                                           m_offsetsInBuffer.end(),
                                           [firstBitsToIgnore](auto offset) {
                                             return offset < firstBitsToIgnore;
                                           }),
                            m_offsetsInBuffer.end());

    std::sort(m_offsetsInBuffer.begin(), m_offsetsInBuffer.end(),
              [](auto a, auto b) { return a > b; });

    m_bufferBitsRead = m_buffer.size() * CHAR_BIT;
    if (!m_offsetsInBuffer.empty()) {
      const auto offset =
          m_nTotalBytesRead * CHAR_BIT + m_offsetsInBuffer.back();
      m_offsetsInBuffer.pop_back();
      return offset;
    }
  }

  return std::numeric_limits<size_t>::max();
}

template <uint8_t bitStringSize>
size_t BitStringFinder<bitStringSize>::refillBuffer() {
  if (!m_fileReader || m_fileReader->eof()) {
    m_nTotalBytesRead += m_buffer.size();
    m_buffer.clear();
    return 0;
  }

  size_t nBytesRead = 0;
  if (m_buffer.empty()) {
    assert(m_nTotalBytesRead == 0);
    assert(m_bufferBitsRead == 0);

    m_buffer.resize(m_fileChunksInBytes);
    nBytesRead = m_fileReader->read(m_buffer.data(), m_buffer.size());
    m_buffer.resize(nBytesRead);
  } else {
    m_nTotalBytesRead += m_buffer.size() - m_movingBytesToKeep;
    m_bufferBitsRead = m_movingBytesToKeep * CHAR_BIT - m_movingBitsToKeep;

    std::memmove(m_buffer.data(),
                 m_buffer.data() + m_buffer.size() - m_movingBytesToKeep,
                 m_movingBytesToKeep);

    const auto nBytesToRead = m_buffer.size() - m_movingBytesToKeep;
    nBytesRead =
        m_fileReader->read(m_buffer.data() + m_movingBytesToKeep, nBytesToRead);
    m_buffer.resize(m_movingBytesToKeep + nBytesRead);
  }

  return nBytesRead;
}
} // namespace rapidgzip
