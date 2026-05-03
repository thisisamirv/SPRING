#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include <AffinityHelpers.hpp>
#include <BlockFinderInterface.hpp>
#include <Cache.hpp>
#include <Prefetcher.hpp>
#include <ThreadPool.hpp>
#include <common.hpp>

#ifdef WITH_PYTHON_SUPPORT
#include <ScopedGIL.hpp>
#endif

namespace rapidgzip {

template <typename T_BlockFinder, typename T_BlockData,
          typename FetchingStrategy>
class BlockFetcher {
public:
  using BlockFinder = T_BlockFinder;
  using BlockData = T_BlockData;
  using BlockCache = Cache<size_t, std::shared_ptr<BlockData>>;

  static_assert(std::is_base_of_v<BlockFinderInterface, BlockFinder>,
                "Block finder must derive from the abstract interface.");

  using GetPartitionOffset = std::function<size_t(size_t)>;

  struct Statistics {
  public:
    [[nodiscard]] double cacheHitRate() const {
      return static_cast<double>(cache.hits + prefetchCache.hits +
                                 prefetchDirectHits) /
             static_cast<double>(gets);
    }

    [[nodiscard]] double uselessPrefetches() const {
      const auto totalFetches = prefetchCount + onDemandFetchCount;
      if (totalFetches == 0) {
        return 0;
      }
      return static_cast<double>(prefetchCache.unusedEntries) /
             static_cast<double>(totalFetches);
    }

    [[nodiscard]] std::string print() const {
      std::stringstream existingBlocks;
      existingBlocks << (blockCountFinalized ? "" : ">=") << blockCount;

      const auto decodeDuration =
          decodeBlockStartTime && decodeBlockEndTime
              ? duration(*decodeBlockStartTime, *decodeBlockEndTime)
              : 0.0;
      const auto optimalDecodeDuration = decodeBlockTotalTime / parallelization;

      const auto poolEfficiency = optimalDecodeDuration / decodeDuration;

      std::stringstream out;
      out << "\n    Parallelization                   : " << parallelization
          << "\n    Cache"
          << "\n        Hits                          : " << cache.hits
          << "\n        Misses                        : " << cache.misses
          << "\n        Unused Entries                : " << cache.unusedEntries
          << "\n        Maximum Fill Size             : " << cache.maxSize
          << "\n        Capacity                      : " << cache.capacity
          << "\n    Prefetch Cache"
          << "\n        Hits                          : " << prefetchCache.hits
          << "\n        Misses                        : "
          << prefetchCache.misses
          << "\n        Unused Entries                : "
          << prefetchCache.unusedEntries
          << "\n        Prefetch Queue Hit            : " << prefetchDirectHits
          << "\n        Maximum Fill Size             : "
          << prefetchCache.maxSize
          << "\n        Capacity                      : "
          << prefetchCache.capacity
          << "\n    Cache Hit Rate                    : "
          << cacheHitRate() * 100 << " %"
          << "\n    Useless Prefetches                : "
          << uselessPrefetches() * 100 << " %"
          << "\n    Access Patterns"
          << "\n        Total Accesses                : " << gets
          << "\n        Duplicate Block Accesses      : "
          << repeatedBlockAccesses
          << "\n        Sequential Block Accesses     : "
          << sequentialBlockAccesses
          << "\n        Block Seeks Back              : "
          << backwardBlockAccesses
          << "\n        Block Seeks Forward           : "
          << forwardBlockAccesses << "\n    Blocks"
          << "\n        Total Existing                : "
          << existingBlocks.str()
          << "\n        Total Fetched                 : "
          << prefetchCount + onDemandFetchCount
          << "\n        Prefetched                    : " << prefetchCount
          << "\n        Fetched On-demand             : " << onDemandFetchCount
          << "\n    Prefetch Stall by BlockFinder     : "
          << waitOnBlockFinderCount << "\n    Time spent in:"
          << "\n        decodeBlock                   : "
          << decodeBlockTotalTime << " s"
          << "\n        std::future::get              : " << futureWaitTotalTime
          << " s"
          << "\n        get                           : " << getTotalTime
          << " s"
          << "\n    Thread Pool Utilization:"
          << "\n        Total Real Decode Duration    : " << decodeDuration
          << " s"
          << "\n        Theoretical Optimal Duration  : "
          << optimalDecodeDuration << " s"
          << "\n        Pool Efficiency (Fill Factor) : "
          << poolEfficiency * 100 << " %";
      return out.str();
    }

    void recordBlockIndexGet(size_t blockindex) {
      ++gets;

      if (!lastAccessedBlock) {
        lastAccessedBlock = blockindex;
      }

      if (blockindex > *lastAccessedBlock + 1) {
        forwardBlockAccesses++;
      } else if (blockindex < *lastAccessedBlock) {
        backwardBlockAccesses++;
      } else if (blockindex == *lastAccessedBlock) {
        repeatedBlockAccesses++;
      } else {
        sequentialBlockAccesses++;
      }

      lastAccessedBlock = blockindex;
    }

  public:
    size_t parallelization{0};
    size_t blockCount{0};
    bool blockCountFinalized{false};

    typename BlockCache::Statistics cache;
    typename BlockCache::Statistics prefetchCache;

    size_t gets{0};
    std::optional<size_t> lastAccessedBlock;
    size_t repeatedBlockAccesses{0};
    size_t sequentialBlockAccesses{0};
    size_t backwardBlockAccesses{0};
    size_t forwardBlockAccesses{0};

    size_t onDemandFetchCount{0};
    size_t prefetchCount{0};
    size_t prefetchDirectHits{0};
    size_t waitOnBlockFinderCount{0};

    std::optional<std::decay_t<decltype(now())>> decodeBlockStartTime;
    std::optional<std::decay_t<decltype(now())>> decodeBlockEndTime;

    double decodeBlockTotalTime{0};
    double futureWaitTotalTime{0};
    double getTotalTime{0};
  };

protected:
  BlockFetcher(std::shared_ptr<BlockFinder> blockFinder, size_t parallelization)
      : m_parallelization(parallelization == 0
                              ? std::max<size_t>(1U, availableCores())
                              : parallelization),
        m_blockFinder(std::move(blockFinder)),
        m_cache(std::max(size_t(16), m_parallelization)),
        m_prefetchCache(2 * m_parallelization),
        m_failedPrefetchCache(m_prefetchCache.capacity()),

        m_threadPool(m_parallelization == 1 ? 0 : m_parallelization) {
    if (!m_blockFinder) {
      throw std::invalid_argument("BlockFinder must be valid!");
    }
    m_statistics.parallelization = m_parallelization;
  }

public:
  virtual ~BlockFetcher() {
    if (m_showProfileOnDestruction) {

      m_cache.shrinkTo(0);
      m_prefetchCache.shrinkTo(0);
      std::cerr << (ThreadSafeOutput()
                    << "[BlockFetcher::~BlockFetcher]" << statistics().print());
    }
  }

  void setStatisticsEnabled(bool enabled) { m_statisticsEnabled = enabled; }

  [[nodiscard]] bool statisticsEnabled() const { return m_statisticsEnabled; }

  void setShowProfileOnDestruction(bool showProfileOnDestruction) {
    m_showProfileOnDestruction = showProfileOnDestruction;
  }

  [[nodiscard]] bool test(const size_t blockOffset) const {
    return (m_prefetching.find(blockOffset) != m_prefetching.end()) ||
           m_cache.test(blockOffset) || m_prefetchCache.test(blockOffset);
  }

  [[nodiscard]] std::shared_ptr<BlockData>
  get(const size_t blockOffset,
      const std::optional<size_t> dataBlockIndex = std::nullopt,
      const GetPartitionOffset &getPartitionOffsetFromOffset = {}) {
    [[maybe_unused]] const auto tGetStart = now();

#ifdef WITH_PYTHON_SUPPORT

    const ScopedGILUnlock unlockedGIL;
#endif

    auto resultFromCaches = getFromCaches(blockOffset);
    auto &cachedResult = resultFromCaches.first;
    auto &queuedResult = resultFromCaches.second;

    const auto validDataBlockIndex =
        dataBlockIndex ? *dataBlockIndex : m_blockFinder->find(blockOffset);
    const auto nextBlockOffset = m_blockFinder->get(validDataBlockIndex + 1);

    if (m_statisticsEnabled) {
      m_statistics.recordBlockIndexGet(validDataBlockIndex);
    }

    if (!cachedResult.has_value() && !queuedResult.valid()) {
      queuedResult = submitOnDemandTask(blockOffset, nextBlockOffset);
    }

    const auto lastFetchedIndex = m_fetchingStrategy.lastFetched();
    m_fetchingStrategy.fetch(validDataBlockIndex);

    const auto resultIsReady = [&cachedResult, &queuedResult]() {
      using namespace std::chrono_literals;
      return cachedResult.has_value() ||
             (queuedResult.valid() &&
              (queuedResult.wait_for(0s) == std::future_status::ready));
    };

    if (!lastFetchedIndex ||
        (lastFetchedIndex.value() != validDataBlockIndex)) {
      prefetchNewBlocks(getPartitionOffsetFromOffset, resultIsReady);
    }

    if (cachedResult.has_value()) {
      assert(!queuedResult.valid());
      if (m_statisticsEnabled) {
        const std::scoped_lock lock(m_analyticsMutex);
        m_statistics.getTotalTime += duration(tGetStart);
      }
      return *std::move(cachedResult);
    }

    [[maybe_unused]] const auto tFutureGetStart = now();
    using namespace std::chrono_literals;

    while (queuedResult.wait_for(1ms) == std::future_status::timeout) {
      prefetchNewBlocks(getPartitionOffsetFromOffset, resultIsReady);
    }
    auto result = std::make_shared<BlockData>(queuedResult.get());
    [[maybe_unused]] const auto futureGetDuration = duration(tFutureGetStart);

    insertIntoCache(blockOffset, result);

    if (m_statisticsEnabled) {
      const std::scoped_lock lock(m_analyticsMutex);
      m_statistics.futureWaitTotalTime += futureGetDuration;
      m_statistics.getTotalTime += duration(tGetStart);
    }

    return result;
  }

  void clearCache() { m_cache.clear(); }

  [[nodiscard]] Statistics statistics() const {
    auto result = m_statistics;
    if (m_blockFinder) {
      result.blockCountFinalized = m_blockFinder->finalized();
      result.blockCount = m_blockFinder->size();
    }
    result.cache = m_cache.statistics();
    result.prefetchCache = m_prefetchCache.statistics();
    return result;
  }

private:
  void insertIntoCache(const size_t blockOffset,
                       std::shared_ptr<BlockData> blockData) {
    if (m_fetchingStrategy.isSequential()) {
      m_cache.clear();
    }
    m_cache.insert(blockOffset, std::move(blockData));
  }

  [[nodiscard]] bool isInCacheOrQueue(const size_t blockOffset) const {
    return (m_prefetching.find(blockOffset) != m_prefetching.end()) ||
           m_cache.test(blockOffset) || m_prefetchCache.test(blockOffset);
  }

  [[nodiscard]] bool isFailedPrefetch(const size_t blockOffset) const {
    const std::scoped_lock lock(m_failedPrefetchCacheMutex);
    return m_failedPrefetchCache.test(blockOffset);
  }

  [[nodiscard]] std::pair<std::optional<std::shared_ptr<BlockData>>,
                          std::future<BlockData>>
  getFromCaches(const size_t blockOffset) {

    auto resultFuture = takeFromPrefetchQueue(blockOffset);

    std::optional<std::shared_ptr<BlockData>> result;
    if (!resultFuture.valid()) {
      result = m_cache.get(blockOffset);
      if (!result) {

        result = m_prefetchCache.get(blockOffset);
        if (result) {
          m_prefetchCache.evict(blockOffset);
          insertIntoCache(blockOffset, *result);
        }
      }
    }

    return std::make_pair(std::move(result), std::move(resultFuture));
  }

  [[nodiscard]] std::future<BlockData>
  takeFromPrefetchQueue(size_t blockOffset) {

    std::future<BlockData> resultFuture;
    const auto match = m_prefetching.find(blockOffset);

    if (match != m_prefetching.end()) {
      resultFuture = std::move(match->second);
      m_prefetching.erase(match);
      assert(resultFuture.valid());

      if (m_statisticsEnabled) {
        ++m_statistics.prefetchDirectHits;
      }
    }

    return resultFuture;
  }

  void processReadyPrefetches() {
    using namespace std::chrono_literals;

    for (auto it = m_prefetching.begin(); it != m_prefetching.end();) {
      auto &[prefetchedBlockOffset, prefetchedFuture] = *it;

      if (prefetchedFuture.valid() &&
          (prefetchedFuture.wait_for(0s) == std::future_status::ready)) {
        BlockData result;
        try {
          result = prefetchedFuture.get();
          m_prefetchCache.insert(
              prefetchedBlockOffset,
              std::make_shared<BlockData>(std::move(result)));
        } catch (...) {

          const std::scoped_lock lock(m_failedPrefetchCacheMutex);
          m_failedPrefetchCache.insert(prefetchedBlockOffset, true);
        }
        it = m_prefetching.erase(it);
      } else {
        ++it;
      }
    }
  }

  void prefetchNewBlocks(const GetPartitionOffset &getPartitionOffsetFromOffset,
                         const std::function<bool()> &stopPrefetching) {

    processReadyPrefetches();

    const auto threadPoolSaturated = [&]() {
      return m_prefetching.size() + 1 >= m_threadPool.capacity();
    };

    if (threadPoolSaturated()) {
      return;
    }

    auto blockIndexesToPrefetch =
        m_fetchingStrategy.prefetch(m_prefetchCache.capacity());

    std::vector<size_t> blockOffsetsToPrefetch(blockIndexesToPrefetch.size());
    for (auto blockIndexToPrefetch : blockIndexesToPrefetch) {

      const auto [blockOffset, _] = m_blockFinder->get(blockIndexToPrefetch, 0);
      if (!blockOffset) {
        continue;
      }

      blockOffsetsToPrefetch.emplace_back(*blockOffset);
      if (getPartitionOffsetFromOffset) {
        const auto partitionOffset = getPartitionOffsetFromOffset(*blockOffset);
        if (*blockOffset != partitionOffset) {
          blockOffsetsToPrefetch.emplace_back(partitionOffset);
        }
      }
    }

    for (auto offset = blockOffsetsToPrefetch.rbegin();
         offset != blockOffsetsToPrefetch.rend(); ++offset) {
      m_prefetchCache.touch(*offset);
      m_cache.touch(*offset);
    }

    for (auto blockIndexToPrefetch : blockIndexesToPrefetch) {
      if (threadPoolSaturated()) {
        break;
      }

      if (m_blockFinder->finalized() &&
          (blockIndexToPrefetch >= m_blockFinder->size())) {
        continue;
      }

      using GetReturnCode = BlockFinderInterface::GetReturnCode;
      std::optional<size_t> prefetchBlockOffset;
      auto prefetchGetReturnCode = GetReturnCode::FAILURE;
      std::optional<size_t> nextPrefetchBlockOffset;
      auto nextPrefetchGetReturnCode = GetReturnCode::FAILURE;
      do {
        std::tie(prefetchBlockOffset, prefetchGetReturnCode) =
            m_blockFinder->get(blockIndexToPrefetch,
                               stopPrefetching() ? 0 : 0.0001);
        std::tie(nextPrefetchBlockOffset, nextPrefetchGetReturnCode) =
            m_blockFinder->get(blockIndexToPrefetch + 1,
                               stopPrefetching() ? 0 : 0.0001);
      } while (!prefetchBlockOffset &&
               (prefetchGetReturnCode != GetReturnCode::FAILURE) &&
               !nextPrefetchBlockOffset &&
               (nextPrefetchGetReturnCode != GetReturnCode::FAILURE) &&
               !stopPrefetching());

      if (m_statisticsEnabled) {
        if (!prefetchBlockOffset.has_value()) {
          m_statistics.waitOnBlockFinderCount++;
        }
      }

      if (!prefetchBlockOffset.has_value() ||
          (prefetchGetReturnCode == GetReturnCode::FAILURE) ||
          !nextPrefetchBlockOffset.has_value() ||
          isInCacheOrQueue(*prefetchBlockOffset) ||
          (getPartitionOffsetFromOffset &&
           isInCacheOrQueue(
               getPartitionOffsetFromOffset(*prefetchBlockOffset))) ||
          isFailedPrefetch(*prefetchBlockOffset)) {
        continue;
      }

      if (const auto offsetToBeEvicted =
              m_prefetchCache.nextNthEviction(m_prefetching.size() + 1);
          offsetToBeEvicted.has_value()) {
        if (contains(blockOffsetsToPrefetch, offsetToBeEvicted)) {
          break;
        }
      }

      ++m_statistics.prefetchCount;
      auto prefetchedFuture = m_threadPool.submit(
          [this, offset = *prefetchBlockOffset,
           nextOffset = *nextPrefetchBlockOffset]() {
            return decodeAndMeasureBlock(offset, nextOffset);
          },
          0);
      const auto [_, wasInserted] = m_prefetching.emplace(
          *prefetchBlockOffset, std::move(prefetchedFuture));
      if (!wasInserted) {
        throw std::logic_error(
            "Submitted future could not be inserted to prefetch queue!");
      }
    }

    if (m_threadPool.unprocessedTasksCount(0) > m_parallelization) {
      throw std::logic_error("The thread pool should not have more tasks than "
                             "there are prefetching futures!");
    }
  }

  [[nodiscard]] std::future<BlockData>
  submitOnDemandTask(const size_t blockOffset,
                     const std::optional<size_t> nextBlockOffset) {
    if (m_statisticsEnabled) {
      ++m_statistics.onDemandFetchCount;
    }
    auto resultFuture = m_threadPool.submit(
        [this, blockOffset, nextBlockOffset]() {
          return decodeAndMeasureBlock(
              blockOffset, nextBlockOffset
                               ? *nextBlockOffset
                               : std::numeric_limits<size_t>::max());
        },
        0);
    assert(resultFuture.valid());
    return resultFuture;
  }

protected:
  [[nodiscard]] virtual BlockData decodeBlock(size_t blockOffset,
                                              size_t nextBlockOffset) const = 0;

  void stopThreadPool() { m_threadPool.stop(); }

  template <class T_Functor,
            std::enable_if_t<std::is_invocable_v<T_Functor>, void> * = nullptr>
  std::future<decltype(std::declval<T_Functor>()())>
  submitTaskWithHighPriority(T_Functor task) {
    return m_threadPool.submit(std::move(task), -1);
  }

  [[nodiscard]] const auto &cache() const noexcept { return m_cache; }

  [[nodiscard]] auto &cache() noexcept { return m_cache; }

  [[nodiscard]] const auto &prefetchCache() const noexcept {
    return m_prefetchCache;
  }

private:
  [[nodiscard]] BlockData decodeAndMeasureBlock(size_t blockOffset,
                                                size_t nextBlockOffset) const {
    [[maybe_unused]] const auto tDecodeStart = now();
    auto blockData = decodeBlock(blockOffset, nextBlockOffset);
    if (m_statisticsEnabled) {
      const auto tDecodeEnd = now();

      const std::scoped_lock lock(m_analyticsMutex);

      const auto &minStartTime = m_statistics.decodeBlockStartTime;
      m_statistics.decodeBlockStartTime.emplace(
          minStartTime ? std::min(*minStartTime, tDecodeStart) : tDecodeStart);

      const auto &maxEndTime = m_statistics.decodeBlockEndTime;
      m_statistics.decodeBlockEndTime.emplace(
          maxEndTime ? std::max(*maxEndTime, tDecodeEnd) : tDecodeEnd);

      m_statistics.decodeBlockTotalTime += duration(tDecodeStart, tDecodeEnd);
    }
    return blockData;
  }

private:
  mutable Statistics m_statistics;
  std::atomic<bool> m_statisticsEnabled{false};
  mutable std::mutex m_analyticsMutex;

protected:
  const size_t m_parallelization;

  FetchingStrategy m_fetchingStrategy;

  bool m_showProfileOnDestruction{false};

private:
  const std::shared_ptr<BlockFinder> m_blockFinder;

  BlockCache m_cache;
  BlockCache m_prefetchCache;
  Cache<size_t, bool> m_failedPrefetchCache;
  mutable std::mutex m_failedPrefetchCacheMutex;

  std::map<size_t, std::future<BlockData>> m_prefetching;
  ThreadPool m_threadPool;
};
} // namespace rapidgzip
