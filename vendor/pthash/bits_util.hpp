#ifndef PTHASH_EXTERNAL_BITS_UTIL_HPP
#define PTHASH_EXTERNAL_BITS_UTIL_HPP

#include <cassert>
#include <cstdint>

#if __cplusplus >= 202002L && !defined(_MSC_VER)
#include <bit>
#endif

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace bits::util {

#if defined(_MSC_VER)
static inline uint64_t msvc_clz32(uint32_t x) {
  unsigned long index;
  _BitScanReverse(&index, x);
  return 31ULL - static_cast<uint64_t>(index);
}

static inline uint64_t msvc_clz64(uint64_t x) {
  unsigned long index;
#if defined(_M_X64) || defined(_M_ARM64)
  _BitScanReverse64(&index, x);
  return 63ULL - static_cast<uint64_t>(index);
#else
  const uint32_t high = static_cast<uint32_t>(x >> 32U);
  if (high != 0) {
    _BitScanReverse(&index, high);
    return 31ULL - static_cast<uint64_t>(index);
  }
  _BitScanReverse(&index, static_cast<uint32_t>(x));
  return 63ULL - static_cast<uint64_t>(index);
#endif
}

static inline uint64_t msvc_ctz32(uint32_t x) {
  unsigned long index;
  _BitScanForward(&index, x);
  return static_cast<uint64_t>(index);
}

static inline uint64_t msvc_ctz64(uint64_t x) {
  unsigned long index;
#if defined(_M_X64) || defined(_M_ARM64)
  _BitScanForward64(&index, x);
  return static_cast<uint64_t>(index);
#else
  const uint32_t low = static_cast<uint32_t>(x);
  if (low != 0) {
    _BitScanForward(&index, low);
    return static_cast<uint64_t>(index);
  }
  _BitScanForward(&index, static_cast<uint32_t>(x >> 32U));
  return 32ULL + static_cast<uint64_t>(index);
#endif
}

static inline uint64_t msvc_popcount64(uint64_t x) {
#if defined(_M_X64) || defined(_M_ARM64)
  return static_cast<uint64_t>(__popcnt64(x));
#else
  return static_cast<uint64_t>(__popcnt(static_cast<uint32_t>(x))) +
         static_cast<uint64_t>(__popcnt(static_cast<uint32_t>(x >> 32U)));
#endif
}
#endif

static inline uint64_t msb(uint32_t x) {
  assert(x > 0);
#if defined(_MSC_VER)
  return msvc_clz32(x);
#else
  return 31 - __builtin_clz(x);
#endif
}
static inline uint64_t msbll(uint64_t x) {
  assert(x > 0);
#if defined(_MSC_VER)
  return msvc_clz64(x);
#else
  return 63 - __builtin_clzll(x);
#endif
}
static inline bool msbll(uint64_t x, uint64_t &ret) {
  if (x) {
#if defined(_MSC_VER)
    ret = msvc_clz64(x);
#else
    ret = 63 - __builtin_clzll(x);
#endif
    return true;
  }
  return false;
}

static inline uint64_t ceil_log2_uint32(uint32_t x) {
  return (x > 1) ? msb(x - 1) + 1 : 0;
}
static inline uint64_t ceil_log2_uint64(uint64_t x) {
  return (x > 1) ? msbll(x - 1) + 1 : 0;
}

static inline uint64_t lsb(uint32_t x) {
  assert(x > 0);
#if defined(_MSC_VER)
  return msvc_ctz32(x);
#else
  return __builtin_ctz(x);
#endif
}
static inline uint64_t lsbll(uint64_t x) {
  assert(x > 0);
#if defined(_MSC_VER)
  return msvc_ctz64(x);
#else
  return __builtin_ctzll(x);
#endif
}
static inline bool lsbll(uint64_t x, uint64_t &ret) {
  if (x) {
#if defined(_MSC_VER)
    ret = msvc_ctz64(x);
#else
    ret = __builtin_ctzll(x);
#endif
    return true;
  }
  return false;
}

static inline uint64_t popcount(uint64_t x) {
#ifdef __SSE4_2__
  return static_cast<uint64_t>(_mm_popcnt_u64(x));
#elif defined(_MSC_VER)
  return msvc_popcount64(x);
#elif __cplusplus >= 202002L
  return std::popcount(x);
#else
  return static_cast<uint64_t>(__builtin_popcountll(x));
#endif
}

static inline uint64_t select_in_word(uint64_t word, uint64_t i) {
  assert(i < popcount(word));
#ifndef __BMI2__

  unsigned int s;
  uint64_t a, b, c, d;
  unsigned int t;
  uint64_t k = popcount(word) - i;
  a = word - ((word >> 1) & ~0UL / 3);
  b = (a & ~0UL / 5) + ((a >> 2) & ~0UL / 5);
  c = (b + (b >> 4)) & ~0UL / 0x11;
  d = (c + (c >> 8)) & ~0UL / 0x101;
  t = (d >> 32) + (d >> 48);
  s = 64;
  s -= ((t - k) & 256) >> 3;
  k -= (t & ((t - k) >> 8));
  t = (d >> (s - 16)) & 0xff;
  s -= ((t - k) & 256) >> 4;
  k -= (t & ((t - k) >> 8));
  t = (c >> (s - 8)) & 0xf;
  s -= ((t - k) & 256) >> 5;
  k -= (t & ((t - k) >> 8));
  t = (b >> (s - 4)) & 0x7;
  s -= ((t - k) & 256) >> 6;
  k -= (t & ((t - k) >> 8));
  t = (a >> (s - 2)) & 0x3;
  s -= ((t - k) & 256) >> 7;
  k -= (t & ((t - k) >> 8));
  t = (word >> (s - 1)) & 0x1;
  s -= ((t - k) & 256) >> 8;
  return s - 1;
#else
  uint64_t k = 1ULL << i;
  asm("pdep %[word], %[mask], %[word]" : [word] "+r"(word) : [mask] "r"(k));
  asm("tzcnt %[bit], %[index]" : [index] "=r"(k) : [bit] "g"(word) : "cc");
  return k;
#endif
}

} // namespace bits::util

#endif
