#ifndef PTHASH_UTILS_UTIL_HPP
#define PTHASH_UTILS_UTIL_HPP

#include <cassert>
#include <chrono>
#include <random>

#if defined(_WIN32)
#include "pthash_windefs.h"
#else
#include <unistd.h>
#endif

#define PTHASH_LIKELY(expr) __builtin_expect((bool)(expr), true)

namespace pthash {

typedef std::chrono::high_resolution_clock clock_type;

namespace constants {

static const char *default_tmp_dirname = ".";

static inline uint64_t available_ram() {
#if defined(_WIN32)
  return static_cast<uint64_t>(sysconf(_SC_PAGESIZE)) *
         static_cast<uint64_t>(sysconf(_SC_PHYS_PAGES));
#else
  return static_cast<uint64_t>(sysconf(_SC_PAGESIZE)) *
         static_cast<uint64_t>(sysconf(_SC_PHYS_PAGES));
#endif
}
static const uint64_t invalid_seed = uint64_t(-1);
static const uint64_t invalid_num_buckets = uint64_t(-1);
static const uint64_t invalid_table_size = uint64_t(-1);
static const double default_alpha = 0.94;

/* for partitioned_phf */
static const uint64_t min_partition_size = 100000;

/* for dense_partitioned_phf */
static const uint64_t log2_table_size_per_partition = 12;
static const uint64_t table_size_per_partition =
    1ULL << log2_table_size_per_partition;

/* For skew_bucketer: a*n keys are placed in b*m buckets */
constexpr float a = 0.6;
constexpr float b = 0.3;
/***********************************************************/

} // namespace constants

static inline uint64_t mul_high(const uint64_t x, const uint64_t y) {
  return (uint64_t)(((__uint128_t)x * (__uint128_t)y) >> 64);
}

static inline uint64_t remap128(const uint64_t hash, const uint64_t n) {
  uint64_t ret = mul_high(hash, n);
  assert(ret < n);
  return ret;
}

static inline uint64_t random_value() {
  unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
  std::mt19937_64 rng(seed);
  return rng();
}

template <typename DurationType> double to_microseconds(DurationType const &d) {
  return static_cast<double>(
      std::chrono::duration_cast<std::chrono::microseconds>(d).count());
}

} // namespace pthash

#endif // PTHASH_UTILS_UTIL_HPP