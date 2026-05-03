#pragma once

#include <vector>

#include "FileReader.hpp"

namespace rapidgzip {
inline int toOrigin(std::ios_base::seekdir anchor) {
  switch (anchor) {
  case std::ios_base::beg:
    return SEEK_SET;
  case std::ios_base::cur:
    return SEEK_CUR;
  case std::ios_base::end:
    return SEEK_END;
  default:
    break;
  }
  return SEEK_SET;
}

class FileReaderStreamBuffer : public std::streambuf {
public:
  static constexpr size_t BUFFER_SIZE = 8ULL * 1024ULL;

  explicit FileReaderStreamBuffer(UniqueFileReader file)
      : m_file(std::move(file)) {
    if (!m_file) {
      throw std::invalid_argument(
          "May only be opened with a valid FileReader!");
    }

    clearGetArea();
  }

  virtual ~FileReaderStreamBuffer() { sync(); }

protected:
  std::streamsize showmanyc() override {
    if (m_file->closed()) {
      return -1;
    }
    return ((gptr() != nullptr) && (gptr() < egptr()))
               ? std::streamsize(egptr() - gptr())
               : 0;
  }

  int_type underflow() override {
    if (m_file->closed()) {
      return traits_type::eof();
    }

    if ((gptr() != nullptr) && (gptr() < egptr())) {
      return traits_type::to_int_type(*gptr());
    }

    const auto nBytesRead = m_file->read(m_buffer.data(), m_buffer.size());
    if (nBytesRead == 0) {
      clearGetArea();
      return traits_type::eof();
    }

    setg(m_buffer.data(), m_buffer.data(), m_buffer.data() + nBytesRead);
    return traits_type::to_int_type(*gptr());
  }

  int_type overflow(int_type = traits_type::eof()) override {
    throw std::runtime_error("Writing is not supported!");
  }

  pos_type seekoff(off_type offset, std::ios_base::seekdir anchor,
                   std::ios_base::openmode mode = std::ios_base::in |
                                                  std::ios_base::out) override {
    if ((mode & std::ios_base::out) != 0) {
      throw std::runtime_error("Writing is not supported!");
    }

    if (anchor == std::ios_base::cur) {

      const auto bufferSize = static_cast<long long int>(egptr() - eback());
      if (bufferSize == 0) {
        return m_file->tell();
      }

      const auto bufferPosition = static_cast<long long int>(gptr() - eback());
      const auto newPosition = bufferPosition + offset;

      if ((newPosition >= 0) && (newPosition <= bufferSize)) {
        setg(eback(), eback() + newPosition, egptr());
        const auto tellOffset = m_file->tell() - (egptr() - gptr());
        return static_cast<pos_type>(tellOffset);
      }

      return seekpos(m_file->tell() - (egptr() - gptr()) + offset, mode);
    }

    clearGetArea();
    return static_cast<pos_type>(m_file->seek(offset, toOrigin(anchor)));
  }

  pos_type seekpos(pos_type offset,
                   std::ios_base::openmode mode = std::ios_base::in |
                                                  std::ios_base::out) override {
    if ((mode & std::ios_base::out) != 0) {
      throw std::runtime_error("Writing is not supported!");
    }

    clearGetArea();
    return static_cast<pos_type>(m_file->seek(offset, SEEK_SET));
  }

private:
  void clearGetArea() {
    setg(m_buffer.data(), m_buffer.data(), m_buffer.data());
  };

protected:
  const UniqueFileReader m_file;
  std::vector<char_type> m_buffer = std::vector<char_type>(BUFFER_SIZE);
};

class FileReaderStream : public FileReaderStreamBuffer, public std::istream {
public:
  explicit FileReaderStream(UniqueFileReader file)
      : FileReaderStreamBuffer(std::move(file)),
        std::istream(static_cast<std::streambuf *>(this)) {}

  [[nodiscard]] bool is_open() const { return !m_file->closed(); }

  void close() const { m_file->close(); }
};
} // namespace rapidgzip
