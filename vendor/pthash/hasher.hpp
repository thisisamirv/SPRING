#ifndef PTHASH_UTILS_HASHER_HPP
#define PTHASH_UTILS_HASHER_HPP

#include <stdexcept>
#include <string>
#include <xxh3.h>

namespace pthash {

namespace util {

struct high_collision_probability_runtime_error : public std::runtime_error {
  high_collision_probability_runtime_error()
      : std::runtime_error("Using 64-bit hash codes with more than 2^30 keys "
                           "can be dangerous due to "
                           "collisions: use 128-bit hash codes instead.") {}
};

template <typename Hasher>
static inline void check_hash_collision_probability(uint64_t size) {

  if (sizeof(typename Hasher::hash_type) * 8 == 64 and size > (1ULL << 30)) {
    throw high_collision_probability_runtime_error();
  }
}

} // namespace util

constexpr uint64_t mix(const uint64_t val) { return val * 0x517cc1b727220a95; }

struct hash64 {
  hash64() = default;
  hash64(uint64_t hash) : m_hash(hash) {}

  [[nodiscard]] uint64_t first() const { return m_hash; }

  [[nodiscard]] uint64_t second() const { return m_hash; }

  [[nodiscard]] uint64_t mix() const { return ::pthash::mix(m_hash); }

private:
  uint64_t m_hash;
};

struct hash128 {
  hash128() = default;
  hash128(XXH128_hash_t xxhash)
      : m_first(xxhash.high64), m_second(xxhash.low64) {}
  hash128(uint64_t first, uint64_t second) : m_first(first), m_second(second) {}

  [[nodiscard]] uint64_t first() const { return m_first; }

  [[nodiscard]] uint64_t second() const { return m_second; }

  [[nodiscard]] uint64_t mix() const { return m_first ^ m_second; }

private:
  uint64_t m_first, m_second;
};

struct xxhash_64 {
  typedef hash64 hash_type;

  static hash64 hash(uint8_t const *begin, uint8_t const *end, uint64_t seed) {
    return XXH64(begin, end - begin, seed);
  }

  static hash64 hash(std::string const &val, uint64_t seed) {
    return XXH64(val.data(), val.size(), seed);
  }

  static hash64 hash(uint64_t const &val, uint64_t seed) {
    return XXH64(&val, sizeof(val), seed);
  }
};

struct xxhash_128 {
  typedef hash128 hash_type;

  static hash128 hash(uint8_t const *begin, uint8_t const *end, uint64_t seed) {
    return XXH128(begin, end - begin, seed);
  }

  static hash128 hash(std::string const &val, uint64_t seed) {
    return XXH128(val.data(), val.size(), seed);
  }

  static hash128 hash(uint64_t const &val, uint64_t seed) {
    return XXH128(&val, sizeof(val), seed);
  }
};

} // namespace pthash

#endif