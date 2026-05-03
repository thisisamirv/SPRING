#ifndef PTHASH_EXTERNAL_BITS_ELIAS_FANO_HPP
#define PTHASH_EXTERNAL_BITS_ELIAS_FANO_HPP

#include "bit_vector.hpp"
#include "compact_vector.hpp"
#include "darray.hpp"

#include <iterator>

namespace bits {

template <

    bool index_zeros = false,

    bool encode_prefix_sum = false,

    typename DArray1 = darray1, typename DArray0 = darray0>
struct elias_fano {
  elias_fano() = default;

  template <typename Iterator>
  void encode(Iterator begin, uint64_t n, uint64_t universe = uint64_t(-1)) {
    if (n == 0)
      return;

    if constexpr (encode_prefix_sum) {
      universe = 0;
      auto tmp = begin;
      for (uint64_t i = 0; i != n; ++i, ++tmp)
        universe += *tmp;
      n = n + 1;
    } else {
      if (universe == uint64_t(-1)) {
        if constexpr (std::is_same_v<typename Iterator::iterator_category,
                                     std::random_access_iterator_tag>) {
          universe = *(begin + (n - 1));
        } else {
          auto tmp = begin;
          for (uint64_t i = 0; i != n - 1; ++i, ++tmp)
            ;
          universe = *tmp;
        }
      }
    }

    uint64_t l = uint64_t((n && universe / n) ? util::msb(universe / n) : 0);

    bit_vector::builder bvb_high_bits(n + (universe >> l) + 1);
    compact_vector::builder cvb_low_bits(n, l);

    const uint64_t low_mask = (uint64_t(1) << l) - 1;
    uint64_t last = 0;

    if constexpr (encode_prefix_sum) {
      if (l)
        cvb_low_bits.set(0, 0);
      bvb_high_bits.set(0, 1);
      n = n - 1;
    }

    for (uint64_t i = 0; i != n; ++i, ++begin) {
      auto v = *begin;
      if constexpr (encode_prefix_sum) {
        v = v + last;
      } else if (i and v < last) {
        std::cerr << "error at " << i << "/" << n << ":\n";
        std::cerr << "last " << last << "\n";
        std::cerr << "current " << v << "\n";
        throw std::runtime_error("sequence is not sorted");
      }
      if (l)
        cvb_low_bits.set(i + encode_prefix_sum, v & low_mask);
      bvb_high_bits.set((v >> l) + i + encode_prefix_sum, 1);
      last = v;
    }

    m_back = last;
    bvb_high_bits.build(m_high_bits);
    cvb_low_bits.build(m_low_bits);
    m_high_bits_d1.build(m_high_bits);
    if constexpr (index_zeros)
      m_high_bits_d0.build(m_high_bits);
  }

  struct iterator {
    iterator() : m_ef(nullptr), m_pos(0), m_l(0), m_val(0) {}

    iterator(elias_fano const *ef, uint64_t pos = 0)
        : m_ef(ef), m_pos(pos), m_l(ef->m_low_bits.width()), m_val(0) {
      if (!has_next() or m_ef->m_high_bits_d1.num_positions() == 0)
        return;
      assert(m_l < 64);
      uint64_t begin = m_ef->m_high_bits_d1.select(m_ef->m_high_bits, m_pos);
      m_high_bits_it = m_ef->m_high_bits.get_iterator_at(begin);
      m_low_bits_it = m_ef->m_low_bits.get_iterator_at(m_pos);
      read_next_value();
    }

    iterator(elias_fano const *ef, uint64_t pos, uint64_t high_hint)
        : m_ef(ef), m_pos(pos), m_l(ef->m_low_bits.width()), m_val(0) {
      if (!has_next() or m_ef->m_high_bits_d1.num_positions() == 0)
        return;
      assert(m_l < 64);
      assert(high_hint == 0 || m_ef->m_high_bits.get(high_hint) == 0);
      m_high_bits_it = m_ef->m_high_bits.get_iterator_at(high_hint);
      m_low_bits_it = m_ef->m_low_bits.get_iterator_at(m_pos);
      read_next_value();
    }

    [[nodiscard]] bool has_next() const { return m_pos < m_ef->size(); }
    [[nodiscard]] bool has_prev() const { return m_pos > 0; }
    [[nodiscard]] uint64_t value() const { return m_val; }
    [[nodiscard]] uint64_t position() const { return m_pos; }

    void next() {
      ++m_pos;
      if (!has_next())
        return;
      read_next_value();
    }

    uint64_t prev_value() {
      assert(m_pos > 0);
      uint64_t pos = m_pos - 1;

      assert(m_high_bits_it.position() >= 2);
      uint64_t high = m_high_bits_it.prev(m_high_bits_it.position() - 2);
      assert(high == m_ef->m_high_bits_d1.select(m_ef->m_high_bits, pos));
      uint64_t low = *(m_low_bits_it - 2);
      return (((high - pos) << m_l) | low);
    }

  private:
    elias_fano const *m_ef;
    uint64_t m_pos;
    uint64_t m_l;
    uint64_t m_val;
    bit_vector::iterator m_high_bits_it;
    compact_vector::iterator m_low_bits_it;

    void read_next_value() {
      assert(m_pos < m_ef->size());
      uint64_t high = m_high_bits_it.next();
      assert(high == m_ef->m_high_bits_d1.select(m_ef->m_high_bits, m_pos));
      uint64_t low = *m_low_bits_it;
      m_val = (((high - m_pos) << m_l) | low);
      ++m_low_bits_it;
    }
  };

  iterator get_iterator_at(uint64_t pos) const { return iterator(this, pos); }
  iterator begin() const { return get_iterator_at(0); }

  [[nodiscard]] uint64_t access(uint64_t i) const {
    assert(i < size());
    return ((m_high_bits_d1.select(m_high_bits, i) - i) << m_low_bits.width()) |
           m_low_bits.access(i);
  }

  [[nodiscard]] uint64_t diff(uint64_t i) const {
    assert(i < size() && encode_prefix_sum);
    uint64_t low1 = m_low_bits.access(i);
    uint64_t low2 = m_low_bits.access(i + 1);
    uint64_t l = m_low_bits.width();
    uint64_t pos = m_high_bits_d1.select(m_high_bits, i);
    uint64_t h1 = pos - i;
    uint64_t h2 = m_high_bits.get_iterator_at(pos + 1).next() - i - 1;
    uint64_t val1 = (h1 << l) | low1;
    uint64_t val2 = (h2 << l) | low2;
    assert(val2 >= val1);
    return val2 - val1;
  }

  struct return_value {
    uint64_t pos;
    uint64_t val;
  };

  return_value next_geq(const uint64_t x) const {
    return next_geq_leftmost(x).first;
  }

  return_value prev_leq(const uint64_t x) const {
    auto [ret, it] = next_geq_rightmost(x);
    if (ret.val > x)
      return {ret.pos - 1, ret.pos != 0 ? it.prev_value() : uint64_t(-1)};
    return ret;
  }

  std::pair<return_value, return_value> locate(const uint64_t x) const {
    auto [lo, it] = next_geq_rightmost(x);
    if (lo.val > x) {
      lo.val = lo.pos != 0 ? it.prev_value() : uint64_t(-1);
      lo.pos -= 1;
    }
    return_value hi{uint64_t(-1), uint64_t(-1)};
    if (lo.pos != size() - 1) {
      hi.pos = lo.pos + 1;
      hi.val = it.value();
      assert(it.position() == hi.pos);
    }
    return {lo, hi};
  }

  [[nodiscard]] uint64_t back() const { return m_back; }
  [[nodiscard]] uint64_t size() const { return m_low_bits.size(); }

  [[nodiscard]] uint64_t num_bytes() const {
    return sizeof(m_back) + m_high_bits.num_bytes() +
           m_high_bits_d1.num_bytes() + m_high_bits_d0.num_bytes() +
           m_low_bits.num_bytes();
  }

  void swap(elias_fano &other) noexcept {
    std::swap(m_back, other.m_back);
    m_high_bits.swap(other.m_high_bits);
    m_high_bits_d1.swap(other.m_high_bits_d1);
    m_high_bits_d0.swap(other.m_high_bits_d0);
    m_low_bits.swap(other.m_low_bits);
  }

  template <typename Visitor> void visit(Visitor &visitor) const {
    visit_impl(visitor, *this);
  }

  template <typename Visitor> void visit(Visitor &visitor) {
    visit_impl(visitor, *this);
  }

private:
  uint64_t m_back = 0;
  bit_vector m_high_bits;
  DArray1 m_high_bits_d1;
  DArray0 m_high_bits_d0;
  compact_vector m_low_bits;

  template <typename Visitor, typename T>
  static void visit_impl(Visitor &visitor, T &&t) {
    visitor.visit(std::forward<T>(t).m_back);
    visitor.visit(std::forward<T>(t).m_high_bits);
    visitor.visit(std::forward<T>(t).m_high_bits_d1);
    visitor.visit(std::forward<T>(t).m_high_bits_d0);
    visitor.visit(std::forward<T>(t).m_low_bits);
  }

  std::pair<return_value, iterator> next_geq_leftmost(const uint64_t x) const {
    static_assert(index_zeros == true, "must build index on zeros");
    assert(m_high_bits_d0.num_positions());

    if (x > back())
      return {{size() - 1, back()}, iterator()};

    uint64_t h_x = x >> m_low_bits.width();
    uint64_t p = 0;
    uint64_t begin = 0;
    if (h_x > 0) {
      p = m_high_bits_d0.select(m_high_bits, h_x - 1);
      begin = p - h_x + 1;
    }
    assert(begin < size());

    auto it = iterator(this, begin, p);
    uint64_t pos = begin;
    uint64_t val = it.value();
    while (val < x) {
      ++pos;

      it.next();
      val = it.value();
    }

    assert(val >= x);
    assert(pos < size());
    assert(val == access(pos));
    assert(it.position() == pos);
    return {{pos, val}, it};
  }

  std::pair<return_value, iterator> next_geq_rightmost(const uint64_t x) const {
    auto [ret, it] = next_geq_leftmost(x);
    if (ret.val == x and ret.pos != size() - 1) {
      assert(it.position() == ret.pos);
      do {
        ++ret.pos;
        if (ret.pos == size())
          break;
        it.next();
        ret.val = it.value();
      } while (ret.val == x);
      assert(ret.val >= x);
      assert(ret.pos > 0);
      ret.pos -= 1;
      ret.val = x;
    }
    return {ret, it};
  }
};

} // namespace bits

#endif