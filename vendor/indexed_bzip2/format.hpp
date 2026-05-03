#pragma once

#include <cstddef>
#include <optional>
#include <utility>

#include <Bgzf.hpp>
#include <FileReader.hpp>
#include <Shared.hpp>
#include <deflate.hpp>
#include <gzip.hpp>

namespace rapidgzip {
[[nodiscard]] inline std::optional<std::pair<FileType, size_t>>
determineFileTypeAndOffset(const UniqueFileReader &fileReader) {
  if (!fileReader) {
    return std::nullopt;
  }

  gzip::BitReader bitReader{fileReader->clone()};
  const auto [gzipHeader, gzipError] = gzip::readHeader(bitReader);
  if (gzipError == Error::NONE) {
    return std::make_pair(blockfinder::Bgzf::isBgzfFile(fileReader)
                              ? FileType::BGZF
                              : FileType::GZIP,
                          bitReader.tell());
  }

  bitReader.seek(0);
  const auto [zlibHeader, zlibError] = zlib::readHeader(bitReader);
  if (zlibError == Error::NONE) {
    return std::make_pair(FileType::ZLIB, bitReader.tell());
  }

  bitReader.seek(0);
  deflate::Block block;
  if (block.readHeader(bitReader) == Error::NONE) {
    return std::make_pair(FileType::DEFLATE, 0);
  }

  return std::nullopt;
}

#ifdef WITH_PYTHON_SUPPORT
[[nodiscard]] std::string determineFileTypeAsString(PyObject *pythonObject) {
  const auto detectedType = determineFileTypeAndOffset(
      ensureSharedFileReader(std::make_unique<PythonFileReader>(pythonObject)));
  return toString(detectedType ? detectedType->first : FileType::NONE);
}
#endif
} // namespace rapidgzip
