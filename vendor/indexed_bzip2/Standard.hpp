#pragma once

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

#include <sys/stat.h>

#ifdef _MSC_VER
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#endif

#include <FileUtils.hpp>
#include <common.hpp>

#include "FileReader.hpp"

namespace rapidgzip {
class StandardFileReader : public FileReader {
public:
  explicit StandardFileReader(std::string filePath)
      : m_file(throwingOpen(filePath, "rb")), m_fileDescriptor(::fileno(fp())),
        m_filePath(std::move(filePath)),
        m_seekable(determineSeekable(m_fileDescriptor)),
        m_fileSizeBytes(std::filesystem::file_size(m_filePath)) {
    init();
  }

#if !defined(__APPLE_CC__) ||                                                  \
    (defined(MAC_OS_X_VERSION_MIN_REQUIRED) &&                                 \
     MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_15)
  explicit StandardFileReader(const std::filesystem::path &filePath)
      : StandardFileReader(filePath.string()) {}

  explicit StandardFileReader(const char *filePath)
      : StandardFileReader(std::string(filePath)) {}
#endif

  explicit StandardFileReader(int fileDescriptor)
      :

        m_file(throwingOpen(dup(fileDescriptor), "rb")),
        m_fileDescriptor(::fileno(fp())),
        m_filePath(fdFilePath(m_fileDescriptor)),
        m_seekable(determineSeekable(m_fileDescriptor)),
        m_fileSizeBytes(fileSize(m_fileDescriptor)) {
    init();
  }

  ~StandardFileReader() override { StandardFileReader::close(); }

  [[nodiscard]] UniqueFileReader cloneRaw() const override {
    throw std::invalid_argument("Cloning file path reader not allowed because "
                                "the internal file position "
                                "should not be modified by multiple owners!");
  }

  void close() override {
    if (!m_file) {
      return;
    }

    if (m_seekable) {
      std::fsetpos(m_file.get(), &m_initialPosition);
    }

    m_file.reset();
  }

  [[nodiscard]] bool closed() const override { return !m_file; }

  [[nodiscard]] bool eof() const override {
    return m_seekable ? m_currentPosition >= m_fileSizeBytes
                      : !m_lastReadSuccessful;
  }

  [[nodiscard]] bool fail() const override { return std::ferror(fp()) != 0; }

  [[nodiscard]] int fileno() const override {
    if (m_file) {
      return m_fileDescriptor;
    }
    throw std::invalid_argument("Trying to get fileno of an invalid file!");
  }

  [[nodiscard]] bool seekable() const override { return m_seekable; }

  [[nodiscard]] size_t read(char *buffer, size_t nMaxBytesToRead) override {
    if (!m_file) {
      throw std::invalid_argument("Invalid or file can't be seeked!");
    }

    if (nMaxBytesToRead == 0) {
      return 0;
    }

    size_t nBytesRead = 0;
    if (buffer == nullptr) {
      if (seekable()) {
        nBytesRead =
            std::min(nMaxBytesToRead, m_fileSizeBytes - m_currentPosition);
        fileSeek(m_file.get(), static_cast<long long int>(nBytesRead),
                 SEEK_CUR);
      } else {
        std::array<char, 16_Ki> tmpBuffer{};
        while (nBytesRead < nMaxBytesToRead) {
          const auto nBytesReadPerCall =
              std::fread(tmpBuffer.data(), 1, tmpBuffer.size(), m_file.get());
          if (nBytesReadPerCall == 0) {
            break;
          }
          nBytesRead += nBytesReadPerCall;
        }
      }
    } else {
      nBytesRead = std::fread(buffer, 1, nMaxBytesToRead, m_file.get());
    }

    if (nBytesRead == 0) {
#if 1

      m_lastReadSuccessful = false;
      return 0;
#else

      std::stringstream message;
      message << "[StandardFileReader] Read call failed (" << nBytesRead
              << " B read)!\n"
              << "  Buffer: " << (void *)buffer << "\n"
              << "  nMaxBytesToRead: " << nMaxBytesToRead << " B\n"
              << "  File pointer: " << (void *)m_file.get() << "\n"
              << "  File size: " << m_fileSizeBytes << " B\n"
              << "  EOF: " << std::feof(m_file.get()) << "\n"
              << "  ferror: " << std::ferror(m_file.get()) << "\n"
              << "  fileno: " << m_fileDescriptor << "\n"
              << "  file path: " << m_filePath << "\n"
              << "  m_currentPosition: " << m_currentPosition << "\n"
              << "  ftell: " << std::ftell(m_file.get()) << "\n"
              << "\n";
      throw std::domain_error(std::move(message).str());
#endif
    }

    m_currentPosition += nBytesRead;
    m_lastReadSuccessful = nBytesRead == nMaxBytesToRead;

    return nBytesRead;
  }

  size_t seek(long long int offset, int origin = SEEK_SET) override {
    if (!m_file || !m_seekable) {
      throw std::invalid_argument("Invalid or file can't be seeked!");
    }

    fileSeek(m_file.get(), offset, origin);

    if (origin == SEEK_SET) {
      m_currentPosition = static_cast<size_t>(std::max(0LL, offset));
    } else {

      m_currentPosition = filePosition(m_file.get());
    }

    return m_currentPosition;
  }

  [[nodiscard]] std::optional<size_t> size() const override {
    return m_fileSizeBytes;
  }

  [[nodiscard]] size_t tell() const override {
    if (m_seekable) {
      return filePosition(fp());
    }
    return m_currentPosition;
  }

  void clearerr() override { std::clearerr(fp()); }

private:
  void init() {
    std::fgetpos(fp(), &m_initialPosition);

    if (m_seekable) {
      StandardFileReader::seek(0, SEEK_SET);
    }
  }

  [[nodiscard]] static bool determineSeekable(int fileNumber) {
    struct stat fileStats{};
    fstat(fileNumber, &fileStats);
    return !S_ISFIFO(fileStats.st_mode);
  }

  [[nodiscard]] FILE *fp() const {
    if (m_file) {
      return m_file.get();
    }
    throw std::invalid_argument("Operation not allowed on an invalid file!");
  }

protected:
  unique_file_ptr m_file;
  const int m_fileDescriptor;
  const std::string m_filePath;

  std::fpos_t m_initialPosition{};
  const bool m_seekable;
  const size_t m_fileSizeBytes;

  size_t m_currentPosition{0};
  bool m_lastReadSuccessful{true};
};

[[nodiscard]] inline UniqueFileReader
openFileOrStdin(const std::string &inputFilePath) {
  if (!inputFilePath.empty()) {
    return std::make_unique<StandardFileReader>(inputFilePath);
  }

#ifdef _MSC_VER
  const auto stdinHandle = _fileno(stdin);
  _setmode(stdinHandle, _O_BINARY);
#else
  const auto stdinHandle = STDIN_FILENO;
#endif

  return std::make_unique<StandardFileReader>(stdinHandle);
}
} // namespace rapidgzip
