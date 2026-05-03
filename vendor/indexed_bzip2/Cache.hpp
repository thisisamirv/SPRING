#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <unordered_map>
#include <utility>

namespace rapidgzip {
namespace CacheStrategy {
template <typename Index> class CacheStrategy {
public:
  virtual ~CacheStrategy() = default;

  virtual void touch(Index index) = 0;

  [[nodiscard]] virtual std::optional<Index> nextEviction() const = 0;

  [[nodiscard]] virtual std::optional<Index>
  nextNthEviction(size_t countToEmplaceHypothetically) const = 0;

  virtual std::optional<Index>
  evict(std::optional<Index> indexToEvict = {}) = 0;
};

template <typename Index>
class LeastRecentlyUsed : public CacheStrategy<Index> {
public:
  using Nonce = uint64_t;

public:
  LeastRecentlyUsed() = default;

  void touch(Index index) override {
    ++usageNonce;
    auto [match, wasInserted] =
        m_lastUsage.try_emplace(std::move(index), usageNonce);
    if (!wasInserted) {
      m_sortedIndexes.erase(match->second);
      match->second = usageNonce;
    }
    m_sortedIndexes.emplace(usageNonce, index);
  }

  [[nodiscard]] std::optional<Index> nextEviction() const override {
    return m_sortedIndexes.empty()
               ? std::nullopt
               : std::make_optional(m_sortedIndexes.begin()->second);
  }

  [[nodiscard]] std::optional<Index>
  nextNthEviction(size_t countToEmplaceHypothetically) const override {
    return (countToEmplaceHypothetically == 0) ||
                   (countToEmplaceHypothetically > m_sortedIndexes.size())
               ? std::nullopt
               : std::make_optional(std::next(m_sortedIndexes.begin(),
                                              countToEmplaceHypothetically - 1)
                                        ->second);
  }

  std::optional<Index> evict(std::optional<Index> indexToEvict = {}) override {
    auto evictedIndex = indexToEvict ? indexToEvict : nextEviction();
    if (evictedIndex) {
      const auto existingEntry = m_lastUsage.find(*evictedIndex);
      if (existingEntry != m_lastUsage.end()) {
        m_sortedIndexes.erase(existingEntry->second);
        m_lastUsage.erase(existingEntry);
      }
    }
    return evictedIndex;
  }

private:
  std::unordered_map<Index, Nonce> m_lastUsage;

  std::map<Nonce, Index> m_sortedIndexes;

  Nonce usageNonce{0};
};
} // namespace CacheStrategy

template <typename Key, typename Value,
          typename CacheStrategy = CacheStrategy::LeastRecentlyUsed<Key>>
class Cache {
public:
  struct Statistics {
    size_t hits{0};
    size_t misses{0};
    size_t unusedEntries{0};
    size_t capacity{0};
    size_t maxSize{0};
  };

public:
  explicit Cache(size_t maxCacheSize) : m_maxCacheSize(maxCacheSize) {}

  [[nodiscard]] std::optional<Value> get(const Key &key) {
    if (const auto match = m_cache.find(key); match != m_cache.end()) {
      ++m_statistics.hits;
      ++m_accesses[key];
      m_cacheStrategy.touch(key);
      return match->second;
    }

    ++m_statistics.misses;
    return std::nullopt;
  }

  void insert(Key key, Value value) {
    if (capacity() == 0) {
      return;
    }

    if (const auto existingEntry = m_cache.find(key);
        existingEntry == m_cache.end()) {
      shrinkTo(capacity() - 1);
      m_cache.emplace(key, std::move(value));
      m_statistics.maxSize = std::max(m_statistics.maxSize, m_cache.size());
    } else {
      existingEntry->second = std::move(value);
    }

    if (const auto match = m_accesses.find(key); match == m_accesses.end()) {
      m_accesses[key] = 0;
    }

    m_cacheStrategy.touch(key);
  }

  void touch(const Key &key) {
    if (test(key)) {
      m_cacheStrategy.touch(key);
    }
  }

  [[nodiscard]] bool test(const Key &key) const {
    return m_cache.find(key) != m_cache.end();
  }

  void clear() { m_cache.clear(); }

  void evict(const Key &key) {
    m_cacheStrategy.evict(key);
    m_cache.erase(key);
  }

  [[nodiscard]] std::optional<Key>
  nextEviction(const std::optional<Key> &key = std::nullopt) const {
    if ((m_cache.size() < capacity()) ||
        (key.has_value() && (m_cache.find(*key) == m_cache.end()))) {
      return std::nullopt;
    }
    return m_cacheStrategy.nextEviction();
  }

  [[nodiscard]] std::optional<Key>
  nextNthEviction(size_t countToBeInserted) const {
    const auto freeCapacity = capacity() - m_cache.size();
    return countToBeInserted <= freeCapacity
               ? std::nullopt
               : m_cacheStrategy.nextNthEviction(countToBeInserted -
                                                 freeCapacity);
  }

  void shrinkTo(size_t newSize) {
    while (m_cache.size() > newSize) {
      const auto toEvict = m_cacheStrategy.evict();
      assert(toEvict);
      const auto keyToEvict = toEvict ? *toEvict : m_cache.begin()->first;
      m_cache.erase(keyToEvict);

      if (const auto match = m_accesses.find(keyToEvict);
          match != m_accesses.end()) {
        if (match->second == 0) {
          m_statistics.unusedEntries++;
        }
        m_accesses.erase(match);
      }
    }
  }

  [[nodiscard]] Statistics statistics() const {
    auto result = m_statistics;
    result.capacity = capacity();
    return result;
  }

  void resetStatistics() {
    m_statistics = Statistics{};
    m_accesses.clear();
  }

  [[nodiscard]] size_t capacity() const { return m_maxCacheSize; }

  [[nodiscard]] size_t size() const { return m_cache.size(); }

  [[nodiscard]] const CacheStrategy &cacheStrategy() const {
    return m_cacheStrategy;
  }

  [[nodiscard]] const auto &contents() const noexcept { return m_cache; }

private:
  CacheStrategy m_cacheStrategy;
  size_t const m_maxCacheSize;
  std::unordered_map<Key, Value> m_cache;

  Statistics m_statistics;
  std::unordered_map<Key, size_t> m_accesses;
};
} // namespace rapidgzip
