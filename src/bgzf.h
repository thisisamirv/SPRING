#ifndef SPRING_BGZF_H_
#define SPRING_BGZF_H_

#include <fstream>
#include <libdeflate.h>
#include <string>
#include <vector>

namespace spring {

class bgzf_ostream {
public:
  bgzf_ostream();
  explicit bgzf_ostream(const std::string &path, int level = 6);
  ~bgzf_ostream();

  bool open(const std::string &path, int level = 6);
  void close();
  bool is_open() const;
  void write(const char *data, std::streamsize size);

private:
  void flush_block();

  std::ofstream out_;
  libdeflate_compressor *compressor_ = nullptr;
  std::vector<char> uncompressed_buffer_;
  std::vector<char> compressed_buffer_;
  int level_ = 6;

  static constexpr size_t kMaxUncompressedBlockSize = 65536;
  // BGZF blocks are limited to 64KB total (compressed).
  // We use a slightly larger buffer for safety.
  static constexpr size_t kMaxCompressedBlockSize = 65536 + 1024;
};

} // namespace spring

#endif
