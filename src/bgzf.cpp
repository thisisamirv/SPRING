// Implements background and multi-threaded BGZF (Block Gzip Format) handling
// used to read and process gzipped FASTQ inputs efficiently.

#include "bgzf.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace spring {

bgzf_ostream::bgzf_ostream() {
  uncompressed_buffer_.reserve(kMaxUncompressedBlockSize);
  compressed_buffer_.resize(kMaxCompressedBlockSize);
}

bgzf_ostream::bgzf_ostream(const std::string &path, int level) : bgzf_ostream() {
  open(path, level);
}

bgzf_ostream::~bgzf_ostream() {
  try {
    close();
  } catch (...) {
    // Destructors should not throw exceptions.
  }
  if (compressor_) {
    libdeflate_free_compressor(compressor_);
  }
}

bool bgzf_ostream::open(const std::string &path, int level) {
  close();
  out_.open(path, std::ios::binary);
  if (!out_)
    return false;
  level_ = level;
  if (!compressor_) {
    compressor_ = libdeflate_alloc_compressor(level);
  }
  return true;
}

void bgzf_ostream::close() {
  if (is_open()) {
    flush_block();
    // Write the empty BGZF terminator block
    // 1f 8b 08 04 00 00 00 00 00 ff 06 00 42 43 02 00 1b 00 03 00 00 00 00 00 00
    // 00 00 00
    static const unsigned char terminator[] = {
        0x1f, 0x8b, 0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
        0xff, 0x06, 0x00, 0x42, 0x43, 0x02, 0x00, 0x1b, 0x00,
        0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00};
    out_.write(reinterpret_cast<const char *>(terminator), sizeof(terminator));
    out_.close();
  }
}

bool bgzf_ostream::is_open() const { return out_.is_open(); }

void bgzf_ostream::write(const char *data, std::streamsize size) {
  size_t remaining = static_cast<size_t>(size);
  const char *ptr = data;
  while (remaining > 0) {
    size_t can_take = kMaxUncompressedBlockSize - uncompressed_buffer_.size();
    if (can_take == 0) {
      flush_block();
      can_take = kMaxUncompressedBlockSize;
    }
    size_t to_take = std::min(remaining, can_take);
    uncompressed_buffer_.insert(uncompressed_buffer_.end(), ptr, ptr + to_take);
    ptr += to_take;
    remaining -= to_take;
  }
}

void bgzf_ostream::flush_block() {
  if (uncompressed_buffer_.empty())
    return;

  size_t compressed_size = libdeflate_deflate_compress(
      compressor_, uncompressed_buffer_.data(), uncompressed_buffer_.size(),
      compressed_buffer_.data(), compressed_buffer_.size());

  if (compressed_size == 0) {
    throw std::runtime_error("BGZF compression failed.");
  }

  // Gzip header (10 bytes)
  unsigned char header[10] = {0x1f, 0x8b, 0x08, 0x04, 0x00,
                              0x00, 0x00, 0x00, 0x00, 0xff};

  // Extra field (8 bytes: XLEN=6, B=42, C=43, SLEN=2, BSIZ)
  uint16_t xlen = 6;
  uint16_t slen = 2;
  uint16_t bsiz = static_cast<uint16_t>(18 + compressed_size + 8 - 1);

  out_.write(reinterpret_cast<const char *>(header), 10);
  out_.write(reinterpret_cast<const char *>(&xlen), 2);
  out_.put('B');
  out_.put('C');
  out_.write(reinterpret_cast<const char *>(&slen), 2);
  out_.write(reinterpret_cast<const char *>(&bsiz), 2);

  // Compressed data
  out_.write(compressed_buffer_.data(), compressed_size);

  // Trailer (8 bytes: CRC32, ISIZE)
  uint32_t crc = libdeflate_crc32(0, uncompressed_buffer_.data(),
                                  uncompressed_buffer_.size());
  uint32_t isize = static_cast<uint32_t>(uncompressed_buffer_.size());
  out_.write(reinterpret_cast<const char *>(&crc), 4);
  out_.write(reinterpret_cast<const char *>(&isize), 4);

  uncompressed_buffer_.clear();
}

} // namespace spring
