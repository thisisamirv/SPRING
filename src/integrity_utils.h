#ifndef SPRING_INTEGRITY_UTILS_H_
#define SPRING_INTEGRITY_UTILS_H_

#include <cstdint>
#include <string>
#include <zlib.h>

namespace spring {

/**
 * @brief Utility for updating a running CRC32 digest with record content.
 *
 * @param crc The running CRC32 value.
 * @param data The string content to add to the digest.
 */
inline void update_record_crc(uint32_t &crc, const std::string &data) {
  crc = static_cast<uint32_t>(crc32(
      static_cast<uLong>(crc), reinterpret_cast<const Bytef *>(data.data()),
      static_cast<uInt>(data.size())));
}

} // namespace spring

#endif // SPRING_INTEGRITY_UTILS_H_
