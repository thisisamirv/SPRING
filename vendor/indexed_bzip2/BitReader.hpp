#pragma once

#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include <cassert>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include <sys/stat.h>

#include <BitManipulation.hpp>
#include <FileReader.hpp>
#include <Shared.hpp>
#include <common.hpp>

namespace rapidgzip {

template <bool MOST_SIGNIFICANT_BITS_FIRST, typename BitBuffer>
class BitReader final : public FileReader {
public:
  static_assert(std::is_unsigned_v<BitBuffer>,
                "Bit buffer type must be unsigned!");

  using bit_count_t = uint32_t;

  static constexpr size_t DEFAULT_BUFFER_REFILL_SIZE = 128_Ki;
  static constexpr int NO_FILE = -1;
  static constexpr auto MAX_BIT_BUFFER_SIZE =
      std::numeric_limits<BitBuffer>::digits;

  class BitReaderException : public std::exception {};

  class BufferNeedsToBeRefilled : public BitReaderException {};

  class EndOfFileReached : public BitReaderException {};

  struct Statistics {
    size_t byteBufferRefillCount{0};
    size_t bitBufferRefillCount{0};
  };

public:
  explicit BitReader(UniqueFileReader fileReader,
                     const size_t bufferRefillSize = DEFAULT_BUFFER_REFILL_SIZE)
      :

        m_file(ensureSharedFileReader(std::move(fileReader))),
        m_bufferRefillSize(bufferRefillSize) {
    if (m_bufferRefillSize == 0) {
      throw std::invalid_argument("The buffer size must be larger than zero!");
    }
  }

  ~BitReader() override = default;

  BitReader(BitReader &&other) noexcept = default;

  BitReader &operator=(BitReader &&other) noexcept = default;

  BitReader &operator=(const BitReader &other) = delete;

  BitReader(const BitReader &other)
      : m_file(other.m_file ? other.m_file->clone() : UniqueFileReader()),
        m_bufferRefillSize(other.m_bufferRefillSize),
        m_inputBuffer(other.m_inputBuffer) {
    if (dynamic_cast<const SharedFileReader *>(other.m_file.get()) == nullptr) {
      throw std::invalid_argument(
          "Cannot copy BitReader if does not contain a SharedFileReader!");
    }

    assert(static_cast<bool>(m_file) == static_cast<bool>(other.m_file));
    if (UNLIKELY(m_file && !m_file->seekable())) [[unlikely]] {
      throw std::invalid_argument(
          "Copying BitReader to unseekable file not supported yet!");
    }
    seek(other.tell());
  }

protected:
  [[nodiscard]] UniqueFileReader cloneRaw() const override {
    return std::make_unique<BitReader>(*this);
  }

public:
  [[nodiscard]] bool fail() const override {
    throw std::logic_error("Not implemented");
  }

  [[nodiscard]] bool eof() const override {
    if (const auto fileSize = size(); seekable() && fileSize.has_value()) {
      return tell() >= *fileSize;
    }
    return (m_inputBufferPosition >= m_inputBuffer.size()) &&
           (!m_file || m_file->eof());
  }

  [[nodiscard]] bool seekable() const override {
    return !m_file || m_file->seekable();
  }

  void close() override {
    m_file.reset();
    m_inputBuffer.clear();
    m_inputBufferPosition = 0;
    clearBitBuffer();
  }

  [[nodiscard]] bool closed() const override {
    return !m_file && m_inputBuffer.empty();
  }

  forceinline BitBuffer read(bit_count_t bitsWanted) {

    assert(bitsWanted > 0);

    assert(bitsWanted < MAX_BIT_BUFFER_SIZE);

    if (LIKELY(bitsWanted <= bitBufferSize())) [[likely]] {
      const auto result = peekUnsafe(bitsWanted);
      seekAfterPeek(bitsWanted);
      return result;
    }

    return read2(bitsWanted);
  }

private:
  BitBuffer read2(bit_count_t bitsWanted) {
    const auto bitsInResult = bitBufferSize();
    assert(bitsWanted >= bitsInResult);
    const auto bitsNeeded = bitsWanted - bitsInResult;
    BitBuffer bits{0};

    if constexpr (MOST_SIGNIFICANT_BITS_FIRST) {
      bits = m_bitBuffer & N_LOWEST_BITS_SET_LUT<BitBuffer>[bitBufferSize()];
    } else {
      bits = bitBufferSize() == 0
                 ? BitBuffer(0)
                 : (m_bitBuffer >> m_bitBufferFree) &
                       N_LOWEST_BITS_SET_LUT<BitBuffer>[bitBufferSize()];
    }

    if constexpr (!MOST_SIGNIFICANT_BITS_FIRST && (ENDIAN != Endian::UNKNOWN)) {
      constexpr bit_count_t BYTES_WANTED = sizeof(BitBuffer);
      constexpr bit_count_t BITS_WANTED = sizeof(BitBuffer) * CHAR_BIT;

      if (LIKELY(m_inputBufferPosition + BYTES_WANTED < m_inputBuffer.size()))
          [[likely]] {
        m_originalBitBufferSize = BITS_WANTED;
        m_bitBufferFree = MAX_BIT_BUFFER_SIZE - BITS_WANTED;
        m_bitBuffer =
            loadUnaligned<BitBuffer>(&m_inputBuffer[m_inputBufferPosition]);

        m_inputBufferPosition += BYTES_WANTED;

        bits |= peekUnsafe(bitsNeeded) << bitsInResult;
        seekAfterPeek(bitsNeeded);

        m_statistics.bitBufferRefillCount++;
        return bits;
      }
    }

    clearBitBuffer();
    try {
      fillBitBuffer();
    } catch (const BufferNeedsToBeRefilled &) {
      refillBuffer();
      try {
        refillBitBuffer();
      } catch (const BufferNeedsToBeRefilled &) {

        if (UNLIKELY(bitsNeeded > bitBufferSize())) [[unlikely]] {
          throw EndOfFileReached();
        }
      }
    }

    if constexpr (MOST_SIGNIFICANT_BITS_FIRST) {
      bits = (bits << bitsNeeded) | peekUnsafe(bitsNeeded);
    } else {
      bits |= peekUnsafe(bitsNeeded) << bitsInResult;
    }
    seekAfterPeek(bitsNeeded);

    return bits;
  }

public:
  forceinline void seekAfterPeek(bit_count_t bitsWanted) {
    assert(bitsWanted <= bitBufferSize());
    m_bitBufferFree += bitsWanted;
  }

  template <uint8_t bitsWanted> forceinline BitBuffer read() {
    if constexpr (bitsWanted == 0) {
      return 0;
    } else {
      static_assert(bitsWanted <= MAX_BIT_BUFFER_SIZE,
                    "Requested bits must fit in buffer!");
      return read(bitsWanted);
    }
  }

  [[nodiscard]] size_t read(char *outputBuffer, size_t nBytesToRead) override {
    const auto oldTell = tell();

    if (UNLIKELY(outputBuffer == nullptr)) [[unlikely]] {
      seek(nBytesToRead, SEEK_CUR);
    } else if (UNLIKELY(oldTell % CHAR_BIT != 0)) [[unlikely]] {
      for (size_t i = 0; i < nBytesToRead; ++i) {
        outputBuffer[i] = static_cast<char>(read(CHAR_BIT));
      }
    } else {
      size_t nBytesRead{0};

      assert(bitBufferSize() % CHAR_BIT == 0);

      for (; (nBytesRead < nBytesToRead) && (bitBufferSize() >= CHAR_BIT);
           ++nBytesRead) {
        outputBuffer[nBytesRead] = static_cast<char>(peekUnsafe(CHAR_BIT));
        seekAfterPeek(CHAR_BIT);
      }

      nBytesRead +=
          readFromBuffer(outputBuffer + nBytesRead, nBytesToRead - nBytesRead);

      const auto nBytesToReadFromFile = nBytesToRead - nBytesRead;
      if (UNLIKELY((nBytesToReadFromFile > 0) && m_file)) [[unlikely]] {
        assert(m_inputBufferPosition == m_inputBuffer.size());
        if (nBytesToRead < std::min<size_t>(1_Ki, m_bufferRefillSize)) {

          refillBuffer();
          readFromBuffer(outputBuffer + nBytesRead, nBytesToRead - nBytesRead);
        } else {
          if ((nBytesToReadFromFile > 0) && m_file) {

            [[maybe_unused]] const auto nBytesReadFromFile =
                m_file->read(outputBuffer + nBytesRead, nBytesToReadFromFile);

            m_inputBufferPosition = 0;
            m_inputBuffer.clear();
          }
        }
      }
    }

    const auto nBitsRead = tell() - oldTell;
    if (UNLIKELY(nBitsRead % CHAR_BIT != 0)) [[unlikely]] {
      throw std::runtime_error("Read not a multiple of CHAR_BIT, probably "
                               "because EOF was encountered!");
    }
    return nBitsRead / CHAR_BIT;
  }

  template <uint8_t bitsWanted> forceinline BitBuffer peek() {
    if constexpr (bitsWanted == 0) {
      return 0;
    } else {
      static_assert(bitsWanted <= MAX_BIT_BUFFER_SIZE,
                    "Requested bits must fit in buffer!");
      return peek(bitsWanted);
    }
  }

private:
  BitBuffer peek2(bit_count_t bitsWanted) {
    assert((bitsWanted <= MAX_BIT_BUFFER_SIZE - (CHAR_BIT - 1)) &&
           "The last 7 bits of the buffer may not be readable because we can "
           "only refill 8-bits at a time.");
    assert(bitsWanted > 0);

    if (UNLIKELY(bitsWanted > bitBufferSize())) [[unlikely]] {
      if constexpr (!MOST_SIGNIFICANT_BITS_FIRST &&
                    (ENDIAN != Endian::UNKNOWN)) {
        if (LIKELY(m_inputBufferPosition + sizeof(BitBuffer) <
                   m_inputBuffer.size())) [[likely]] {

          if (bitBufferSize() == 0) {
            m_originalBitBufferSize = sizeof(BitBuffer) * CHAR_BIT;
            m_bitBufferFree =
                MAX_BIT_BUFFER_SIZE - sizeof(BitBuffer) * CHAR_BIT;
            m_bitBuffer =
                loadUnaligned<BitBuffer>(&m_inputBuffer[m_inputBufferPosition]);

            m_inputBufferPosition += sizeof(BitBuffer);
            return peekUnsafe(bitsWanted);
          }

          const auto shrinkedBitBufferSize =
              ceilDiv(bitBufferSize(), CHAR_BIT) * CHAR_BIT;
          const auto bitsToLoad = MAX_BIT_BUFFER_SIZE - shrinkedBitBufferSize;
          const auto bytesToLoad = bitsToLoad / CHAR_BIT;

          const auto bytesToAppend =
              loadUnaligned<BitBuffer>(&m_inputBuffer[m_inputBufferPosition]);
          m_bitBuffer = (m_bitBuffer >> bitsToLoad) |
                        (bytesToAppend << (MAX_BIT_BUFFER_SIZE - bitsToLoad));

          m_originalBitBufferSize = MAX_BIT_BUFFER_SIZE;
          m_bitBufferFree -= bitsToLoad;
          m_inputBufferPosition += bytesToLoad;

          return peekUnsafe(bitsWanted);
        }
      }

      try {

        if constexpr (!MOST_SIGNIFICANT_BITS_FIRST &&
                      (ENDIAN != Endian::UNKNOWN)) {

          refillBitBuffer();
        } else {
          if (bitBufferSize() == 0) {
            m_bitBuffer = 0;
            m_originalBitBufferSize = 0;
          } else {
            shrinkBitBuffer();

            if constexpr (!MOST_SIGNIFICANT_BITS_FIRST) {
              m_bitBuffer >>= static_cast<uint8_t>(MAX_BIT_BUFFER_SIZE -
                                                   m_originalBitBufferSize);
            }
          }

          fillBitBuffer();
        }
      } catch (const BufferNeedsToBeRefilled &) {
        refillBuffer();
        try {
          refillBitBuffer();
        } catch (const BufferNeedsToBeRefilled &) {

          if (UNLIKELY(bitsWanted > bitBufferSize())) [[unlikely]] {
            throw EndOfFileReached();
          }
        }
      }
    }

    return peekUnsafe(bitsWanted);
  }

public:
  forceinline BitBuffer peek(bit_count_t bitsWanted) {
    if (UNLIKELY(bitsWanted > bitBufferSize())) [[unlikely]] {
      return peek2(bitsWanted);
    }
    return peekUnsafe(bitsWanted);
  }

  [[nodiscard]] std::pair<BitBuffer, size_t> peekAvailable() const {
    return {peekUnsafe(bitBufferSize()), bitBufferSize()};
  }

  [[nodiscard]] size_t tell() const override {

    size_t position = m_inputBufferPosition * CHAR_BIT;

    if (m_file) {
      const auto filePosition = m_file->tell();
      if (UNLIKELY(static_cast<size_t>(filePosition) < m_inputBuffer.size()))
          [[unlikely]] {
        throw std::logic_error("The byte buffer should not contain more data "
                               "than the file position!");
      }
      position += (filePosition - m_inputBuffer.size()) * CHAR_BIT;
    }

    if (UNLIKELY(position < bitBufferSize())) [[unlikely]] {
      throw std::logic_error("The bit buffer should not contain more data than "
                             "have been read from the file!");
    }

    return position - bitBufferSize();
  }

  void clearerr() override {
    if (m_file) {
      m_file->clearerr();
    }
  }

public:
  [[nodiscard]] int fileno() const override {
    if (UNLIKELY(!m_file)) [[unlikely]] {
      throw std::invalid_argument("The file is not open!");
    }
    return m_file->fileno();
  }

  size_t seek(long long int offsetBits, int origin = SEEK_SET) override;

  [[nodiscard]] std::optional<size_t> size() const override {
    auto sizeInBytes = m_inputBuffer.size();
    if (m_file) {
      const auto fileSize = m_file->size();
      if (!fileSize) {
        return std::nullopt;
      }
      sizeInBytes = *fileSize;
    }
    return sizeInBytes * CHAR_BIT;
  }

  [[nodiscard]] const std::vector<std::uint8_t> &buffer() const {
    return m_inputBuffer;
  }

  [[nodiscard]] constexpr uint64_t bufferRefillSize() const {
    return m_bufferRefillSize;
  }

  [[nodiscard]] constexpr Statistics statistics() const { return m_statistics; }

private:
  size_t fullSeek(size_t offsetBits);

  void refillBuffer() {
    if (UNLIKELY(!m_file)) [[unlikely]] {
      throw std::logic_error(
          "Can not refill buffer with data from non-existing file!");
    }

    const auto oldBufferSize = m_inputBuffer.size();
    m_inputBuffer.resize(m_bufferRefillSize);
    const auto nBytesRead = m_file->read(
        reinterpret_cast<char *>(m_inputBuffer.data()), m_inputBuffer.size());
    if (nBytesRead == 0) {
      m_inputBuffer.resize(oldBufferSize);
      return;
    }

    m_inputBuffer.resize(nBytesRead);
    m_inputBufferPosition = 0;

    m_statistics.byteBufferRefillCount++;
  }

  void shrinkBitBuffer() {
    if (m_originalBitBufferSize == bitBufferSize()) {
      return;
    }

    assert((m_originalBitBufferSize % CHAR_BIT == 0) &&
           "Not necessary but should be true because we only load byte-wise "
           "and only shrink byte-wise!");
    assert(m_originalBitBufferSize >= bitBufferSize());
    assert(m_originalBitBufferSize >=
           ceilDiv(bitBufferSize(), CHAR_BIT) * CHAR_BIT);

    m_originalBitBufferSize = ceilDiv(bitBufferSize(), CHAR_BIT) * CHAR_BIT;

    if constexpr (MOST_SIGNIFICANT_BITS_FIRST) {
      m_bitBuffer &= N_LOWEST_BITS_SET_LUT<BitBuffer>[m_originalBitBufferSize];
    } else {
      m_bitBuffer &= N_HIGHEST_BITS_SET_LUT<BitBuffer>[m_originalBitBufferSize];
    }
  }

  size_t readFromBuffer(void *const outputBuffer, size_t const nBytesToRead) {
    const auto nBytesReadFromBuffer =
        std::min(nBytesToRead, m_inputBuffer.size() - m_inputBufferPosition);
    if (nBytesReadFromBuffer > 0) {
      std::memcpy(outputBuffer, m_inputBuffer.data() + m_inputBufferPosition,
                  nBytesReadFromBuffer);
      m_inputBufferPosition += nBytesReadFromBuffer;
    }
    return nBytesReadFromBuffer;
  }

  void refillBitBuffer() {

    if (bitBufferSize() + CHAR_BIT > MAX_BIT_BUFFER_SIZE) {
      return;
    }

    if (bitBufferSize() == 0) {
      m_bitBuffer = 0;
      m_originalBitBufferSize = 0;
    } else {
      shrinkBitBuffer();

      if constexpr (!MOST_SIGNIFICANT_BITS_FIRST) {
        assert(m_originalBitBufferSize > 0);

        m_bitBuffer >>=
            static_cast<uint8_t>(MAX_BIT_BUFFER_SIZE - m_originalBitBufferSize);
      }
    }

    fillBitBuffer();
  }

  void fillBitBuffer() {

    struct ShiftBackOnReturn {
      ShiftBackOnReturn(BitBuffer &bitBuffer,
                        const uint8_t &originalBitBufferSize) noexcept
          : m_bitBuffer(bitBuffer),
            m_originalBitBufferSize(originalBitBufferSize) {}

      ~ShiftBackOnReturn() noexcept {

        if constexpr (!MOST_SIGNIFICANT_BITS_FIRST) {
          if (m_originalBitBufferSize > 0) {
            m_bitBuffer <<= static_cast<uint8_t>(MAX_BIT_BUFFER_SIZE -
                                                 m_originalBitBufferSize);
          }
        }
      }

    private:
      BitBuffer &m_bitBuffer;
      const uint8_t &m_originalBitBufferSize;
    } const shiftBackOnExit(m_bitBuffer, m_originalBitBufferSize);

    for (; m_originalBitBufferSize + CHAR_BIT <= MAX_BIT_BUFFER_SIZE;
         m_bitBufferFree -= CHAR_BIT, m_originalBitBufferSize += CHAR_BIT) {
      if (UNLIKELY(m_inputBufferPosition >= m_inputBuffer.size()))
          [[unlikely]] {
        throw BufferNeedsToBeRefilled();
      }

      if constexpr (MOST_SIGNIFICANT_BITS_FIRST) {
        m_bitBuffer <<= static_cast<uint8_t>(CHAR_BIT);
        m_bitBuffer |=
            static_cast<BitBuffer>(m_inputBuffer[m_inputBufferPosition++]);
      } else {
        m_bitBuffer |=
            (static_cast<BitBuffer>(m_inputBuffer[m_inputBufferPosition++])
             << m_originalBitBufferSize);
      }
    }

    m_statistics.bitBufferRefillCount++;
  }

  [[nodiscard]] forceinline BitBuffer peekUnsafe(bit_count_t bitsWanted) const {
    assert(bitsWanted <= bitBufferSize());
    assert(bitsWanted > 0);

    if constexpr (MOST_SIGNIFICANT_BITS_FIRST) {
      return (m_bitBuffer >> (bitBufferSize() - bitsWanted)) &
             N_LOWEST_BITS_SET_LUT<BitBuffer>[bitsWanted];
    } else {
      assert(bitBufferSize() > 0);

      return (m_bitBuffer >> m_bitBufferFree) &
             N_LOWEST_BITS_SET_LUT<BitBuffer>[bitsWanted];
    }
  }

  forceinline void clearBitBuffer() {
    m_originalBitBufferSize = 0;
    m_bitBufferFree = MAX_BIT_BUFFER_SIZE;
    m_bitBuffer = 0;
  }

private:
  [[nodiscard]] forceinline constexpr auto bitBufferSize() const noexcept {
    return MAX_BIT_BUFFER_SIZE - m_bitBufferFree;
  }

private:
  UniqueFileReader m_file;

  size_t m_bufferRefillSize{DEFAULT_BUFFER_REFILL_SIZE};
  std::vector<uint8_t> m_inputBuffer;
  size_t m_inputBufferPosition = 0;

  Statistics m_statistics;

public:
  BitBuffer m_bitBuffer = 0;

  uint32_t m_bitBufferFree{MAX_BIT_BUFFER_SIZE};

  uint8_t m_originalBitBufferSize = 0;
};

template <bool MOST_SIGNIFICANT_BITS_FIRST, typename BitBuffer>
inline size_t BitReader<MOST_SIGNIFICANT_BITS_FIRST, BitBuffer>::seek(
    long long int offsetBits, int origin) {
  if (origin == SEEK_END) {
    const auto fileSize = size();
    if (!fileSize.has_value()) {
      if (!m_file) {
        throw std::logic_error("File has already been closed!");
      }

      if (!m_file->seekable()) {
        throw std::logic_error("File is not seekable!");
      }

      const auto realFileSize =
          static_cast<long long int>(m_file->seek(0, SEEK_END));

      const auto absoluteOffset =
          saturatingAddition(std::min(offsetBits, 0LL), realFileSize);
      return fullSeek(static_cast<size_t>(std::max(absoluteOffset, 0LL)));
    }
  }

  const auto positiveOffsetBits = effectiveOffset(offsetBits, origin);

  if (positiveOffsetBits == tell()) {
    return positiveOffsetBits;
  }

  if (!seekable() && (positiveOffsetBits < tell())) {
    std::stringstream message;
    message << "File is not seekable! Requested to seek to "
            << formatBits(positiveOffsetBits)
            << ". Currently at: " << formatBits(tell());
    throw std::invalid_argument(std::move(message).str());
  }

  if (!m_file) {
    throw std::logic_error("File has already been closed!");
  }

  if (positiveOffsetBits >= tell()) {
    const auto relativeOffsets = positiveOffsetBits - tell();

    if (static_cast<size_t>(relativeOffsets) <= bitBufferSize()) {
      seekAfterPeek(static_cast<decltype(bitBufferSize())>(relativeOffsets));
      return positiveOffsetBits;
    }

    const auto stillToSeek = relativeOffsets - bitBufferSize();
    const auto newInputBufferPosition =
        m_inputBufferPosition + stillToSeek / CHAR_BIT;
    if (newInputBufferPosition <= m_inputBuffer.size()) {
      clearBitBuffer();

      m_inputBufferPosition = newInputBufferPosition;
      if (stillToSeek % CHAR_BIT > 0) {
        read(stillToSeek % CHAR_BIT);
      }

      return positiveOffsetBits;
    }
  } else {
    const auto relativeOffsets = tell() - positiveOffsetBits;

    if (relativeOffsets + bitBufferSize() <= m_originalBitBufferSize) {
      m_bitBufferFree -=
          static_cast<decltype(bitBufferSize())>(relativeOffsets);
      return positiveOffsetBits;
    }

    const auto seekBackWithBuffer = relativeOffsets + bitBufferSize();
    const auto bytesToSeekBack =
        static_cast<size_t>(ceilDiv(seekBackWithBuffer, CHAR_BIT));

    if (bytesToSeekBack <= m_inputBufferPosition) {
      m_inputBufferPosition -=
          static_cast<decltype(m_inputBufferPosition)>(bytesToSeekBack);
      clearBitBuffer();

      const auto bitsToSeekForward =
          bytesToSeekBack * CHAR_BIT - seekBackWithBuffer;
      if (bitsToSeekForward > 0) {
        read(static_cast<uint8_t>(bitsToSeekForward));
      }

      return positiveOffsetBits;
    }
  }

  return fullSeek(positiveOffsetBits);
}

template <bool MOST_SIGNIFICANT_BITS_FIRST, typename BitBuffer>
inline size_t
BitReader<MOST_SIGNIFICANT_BITS_FIRST, BitBuffer>::fullSeek(size_t offsetBits) {
  if (!m_file) {
    throw std::logic_error("File has already been closed!");
  }

  const auto bytesToSeek = offsetBits >> 3U;
  const auto subBitsToSeek = static_cast<bit_count_t>(offsetBits & 7U);

  clearBitBuffer();

  m_inputBuffer.clear();
  m_inputBufferPosition = 0;

  if (seekable()) {
    const auto newPosition =
        m_file->seek(static_cast<long long int>(bytesToSeek), SEEK_SET);

    if ((m_file->eof() &&
         (!m_file->seekable() || (m_file->tell() > m_file->size()))) ||
        m_file->fail()) {
      std::stringstream msg;
      msg << "[BitReader] Could not seek to specified byte " << bytesToSeek
          << " subbit " << static_cast<int>(subBitsToSeek)
          << ", SharedFileReader: "
          << (dynamic_cast<SharedFileReader *>(m_file.get()) != nullptr)
          << ", SinglePassFileReader: "
          << (dynamic_cast<SinglePassFileReader *>(m_file.get()) != nullptr)
          << ", tell: " << m_file->tell()
          << ", size: " << m_file->size().value_or(0)
          << ", feof: " << m_file->eof() << ", ferror: " << m_file->fail()
          << ", newPosition: " << newPosition;
      throw std::invalid_argument(std::move(msg).str());
    }
  } else if (offsetBits < tell()) {
    throw std::logic_error(
        "Can not emulate backward seeking on non-seekable file!");
  } else {

    throw std::logic_error(
        "Seeking forward on non-seekable input is an unfinished feature!");
  }

  if (subBitsToSeek > 0) {
    read(subBitsToSeek);
  }

  return offsetBits;
}
} // namespace rapidgzip
