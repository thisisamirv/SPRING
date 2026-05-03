#ifndef PTHASH_UTILS_BUCKETERS_HPP
#define PTHASH_UTILS_BUCKETERS_HPP

#include "pthash_util.hpp"
#include <stdexcept>
#include <utility>

namespace pthash {

struct opt_bucketer {
  opt_bucketer() = default;

  void init(const uint64_t num_buckets) { m_num_buckets = num_buckets; }

  [[nodiscard]] uint64_t bucket(uint64_t hash) const {

    uint64_t H =
        mul_high(mul_high(hash, hash), (hash >> 1) | (1ULL << 63)) / 8 * 7 +
        hash / 8;
    return remap128(H, m_num_buckets);
  }

  [[nodiscard]] uint64_t num_buckets() const { return m_num_buckets; }

  [[nodiscard]] uint64_t num_bits() const { return 8 * sizeof(m_num_buckets); }

  void swap(opt_bucketer &other) noexcept {
    std::swap(m_num_buckets, other.m_num_buckets);
  }

  template <typename Visitor> void visit(Visitor &visitor) const {
    visit_impl(visitor, *this);
  }

  template <typename Visitor> void visit(Visitor &visitor) {
    visit_impl(visitor, *this);
  }

private:
  template <typename Visitor, typename T>
  static void visit_impl(Visitor &visitor, T &&t) {
    visitor.visit(std::forward<T>(t).m_num_buckets);
  }

  uint64_t m_num_buckets = 0;
};

struct skew_bucketer {
  skew_bucketer() = default;

  void init(const uint64_t num_buckets) {
    m_num_dense_buckets = constants::b * num_buckets;
    m_num_sparse_buckets = num_buckets - m_num_dense_buckets;
  }

  [[nodiscard]] uint64_t bucket(uint64_t hash) const {
    static const uint64_t T = constants::a * static_cast<double>(UINT64_MAX);
    uint64_t H = hash << 32;
    return (hash < T) ? remap128(H, m_num_dense_buckets)
                      : m_num_dense_buckets + remap128(H, m_num_sparse_buckets);
  }

  [[nodiscard]] uint64_t num_buckets() const {
    return m_num_dense_buckets + m_num_sparse_buckets;
  }

  [[nodiscard]] uint64_t num_bits() const {
    return 8 * (sizeof(m_num_dense_buckets) + sizeof(m_num_sparse_buckets));
  }

  void swap(skew_bucketer &other) noexcept {
    std::swap(m_num_dense_buckets, other.m_num_dense_buckets);
    std::swap(m_num_sparse_buckets, other.m_num_sparse_buckets);
  }

  template <typename Visitor> void visit(Visitor &visitor) const {
    visit_impl(visitor, *this);
  }

  template <typename Visitor> void visit(Visitor &visitor) {
    visit_impl(visitor, *this);
  }

private:
  template <typename Visitor, typename T>
  static void visit_impl(Visitor &visitor, T &&t) {
    visitor.visit(std::forward<T>(t).m_num_dense_buckets);
    visitor.visit(std::forward<T>(t).m_num_sparse_buckets);
  }

  uint64_t m_num_dense_buckets = 0, m_num_sparse_buckets = 0;
};

struct range_bucketer {
  range_bucketer() = default;

  void init(const uint64_t num_buckets) {
    if (num_buckets > (1ULL << 32))
      throw std::runtime_error("too many buckets");
    m_num_buckets = num_buckets;
  }

  [[nodiscard]] uint64_t bucket(const uint64_t hash) const {
    return ((hash >> 32) * m_num_buckets) >> 32;
  }

  [[nodiscard]] uint64_t num_buckets() const { return m_num_buckets; }

  [[nodiscard]] uint64_t num_bits() const { return 8 * sizeof(m_num_buckets); }

  void swap(range_bucketer &other) noexcept {
    std::swap(m_num_buckets, other.m_num_buckets);
  }

  template <typename Visitor> void visit(Visitor &visitor) const {
    visit_impl(visitor, *this);
  }

  template <typename Visitor> void visit(Visitor &visitor) {
    visit_impl(visitor, *this);
  }

private:
  template <typename Visitor, typename T>
  static void visit_impl(Visitor &visitor, T &&t) {
    visitor.visit(std::forward<T>(t).m_num_buckets);
  }

  uint64_t m_num_buckets = 0;
};

} // namespace pthash

#endif