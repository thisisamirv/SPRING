#pragma once

#include <cstddef>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include <common.hpp>

namespace rapidgzip {
class FileReader;

using UniqueFileReader = std::unique_ptr<FileReader>;

class FileReader {
public:
  FileReader() = default;

  virtual ~FileReader() = default;

  FileReader(const FileReader &) = delete;

  FileReader &operator=(const FileReader &) = delete;

  FileReader(FileReader &&) = default;

  FileReader &operator=(FileReader &&) = delete;

  [[nodiscard]] UniqueFileReader clone() const {
    auto fileReader = cloneRaw();
    if (!fileReader->closed() && (fileReader->tell() != tell())) {
      fileReader->seekTo(tell());
    }
    return fileReader;
  }

  virtual void close() = 0;

  [[nodiscard]] virtual bool closed() const = 0;

  [[nodiscard]] virtual bool eof() const = 0;

  [[nodiscard]] virtual bool fail() const = 0;

  [[nodiscard]] virtual int fileno() const = 0;

  [[nodiscard]] virtual bool seekable() const = 0;

  [[nodiscard]] virtual size_t read(char *buffer, size_t nMaxBytesToRead) = 0;

  virtual size_t seek(long long int offset, int origin = SEEK_SET) = 0;

  size_t seekTo(uint64_t offset) {
    if (offset >
        static_cast<uint64_t>(std::numeric_limits<long long int>::max())) {
      throw std::invalid_argument("Value " + std::to_string(offset) +
                                  " out of range of long long int!");
    }
    return seek(static_cast<long long int>(offset));
  }

  [[nodiscard]] virtual std::optional<size_t> size() const = 0;

  [[nodiscard]] virtual size_t tell() const = 0;

  virtual void clearerr() = 0;

protected:
  [[nodiscard]] virtual UniqueFileReader cloneRaw() const {
    throw std::logic_error("Not implemented!");
  }

  [[nodiscard]] size_t effectiveOffset(long long int offset, int origin) const {
    offset = [&]() {
      switch (origin) {
      case SEEK_CUR:
        return saturatingAddition(static_cast<long long int>(tell()), offset);
      case SEEK_SET:
        return offset;
      case SEEK_END:
        if (const auto fileSize = size(); fileSize.has_value()) {
          return saturatingAddition(static_cast<long long int>(*fileSize),
                                    offset);
        }
        throw std::logic_error("File size is not available to seek from end!");
      default:
        break;
      }
      throw std::invalid_argument("Invalid seek origin supplied: " +
                                  std::to_string(origin));
    }();

    const auto positiveOffset = static_cast<size_t>(std::max(offset, 0LL));

    const auto fileSize = size();
    return fileSize.has_value() ? std::min(positiveOffset, *fileSize)
                                : positiveOffset;
  }
};
} // namespace rapidgzip
