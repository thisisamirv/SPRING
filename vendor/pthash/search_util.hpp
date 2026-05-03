
#ifndef PTHASH_BUILDERS_SEARCH_UTIL_HPP
#define PTHASH_BUILDERS_SEARCH_UTIL_HPP

#include "essentials.hpp"
#include "hasher.hpp"
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace pthash {

constexpr uint64_t search_cache_size = 1000;

template <size_t... Indices>
constexpr std::array<uint64_t, sizeof...(Indices)>
create_cache(std::index_sequence<Indices...>) {
  return {mix(Indices)...};
}

constexpr auto hashed_pilots_cache =
    create_cache(std::make_index_sequence<search_cache_size>{});

struct search_logger {
  search_logger(uint64_t num_keys, uint64_t num_buckets)
      : m_num_keys(num_keys), m_num_buckets(num_buckets),
        m_step(m_num_buckets > 20 ? m_num_buckets / 20 : 1) {}

  void init() {
    essentials::logger("search starts");
    m_timer.start();
  }

  void update(uint64_t bucket, uint64_t bucket_size) {
    m_placed_keys += bucket_size;
    if (bucket > 0 and bucket % m_step == 0)
      print(bucket);
  }

  void finalize(uint64_t bucket) {
    m_step = bucket - m_bucket;
    print(bucket);
    essentials::logger("search ends");
    std::cout << " == " << m_num_buckets - bucket << " empty buckets ("
              << ((m_num_buckets - bucket) * 100.0) / m_num_buckets << "%)"
              << std::endl;
  }

  [[nodiscard]] uint64_t num_pilots() const { return m_num_pilots; }
  void set_num_pilots(uint64_t v) { m_num_pilots = v; }

  [[nodiscard]] uint64_t num_large_pilots() const { return m_num_large_pilots; }
  void set_num_large_pilots(uint64_t v) { m_num_large_pilots = v; }

private:
  uint64_t m_num_keys;
  uint64_t m_num_buckets;
  uint64_t m_step;
  uint64_t m_bucket = 0;
  uint64_t m_placed_keys = 0;
  uint64_t m_num_pilots = 0;
  uint64_t m_num_large_pilots = 0;

  essentials::timer<std::chrono::high_resolution_clock,
                    std::chrono::milliseconds>
      m_timer;

  void print(uint64_t bucket) {
    m_timer.stop();
    std::stringbuf buffer;
    std::ostream os(&buffer);
    os << m_step << " buckets done in " << std::fixed << std::setprecision(2)
       << m_timer.elapsed() / 1000 << " seconds ("
       << (m_placed_keys * 100.0) / m_num_keys << "% of keys, "
       << (bucket * 100.0) / m_num_buckets << "% of buckets)";
    essentials::logger(buffer.str());
    m_bucket = bucket;
    m_timer.reset();
    m_timer.start();
  }
};

} // namespace pthash

#endif
