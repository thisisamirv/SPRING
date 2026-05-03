#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#include <BlockFinderInterface.hpp>
#include <JoiningThread.hpp>
#include <StreamedResults.hpp>
#ifdef WITH_PYTHON_SUPPORT
#include <ScopedGIL.hpp>
#endif

namespace rapidgzip {

template <typename T_RawBlockFinder>
class BlockFinder final : public BlockFinderInterface {
public:
  using RawBlockFinder = T_RawBlockFinder;
  using BlockOffsets = StreamedResults<size_t>::Values;

public:
  explicit BlockFinder(std::unique_ptr<RawBlockFinder> rawBlockFinder)
      : m_rawBlockFinder(std::move(rawBlockFinder)) {}

  ~BlockFinder() override {
    const std::scoped_lock lock(m_mutex);
    m_cancelThread = true;
    m_changed.notify_all();
  }

public:
  void startThreads() {
    if (!m_rawBlockFinder) {
      throw std::invalid_argument("You may not start the block finder without "
                                  "a valid bit string finder!");
    }

    if (!m_blockFinder) {
      m_blockFinder =
          std::make_unique<JoiningThread>([this]() { blockFinderMain(); });
    }
  }

  void stopThreads() {
    {
      const std::scoped_lock lock(m_mutex);
      m_cancelThread = true;
      m_changed.notify_all();
    }

    if (m_blockFinder && m_blockFinder->joinable()) {
      m_blockFinder->join();
    }
  }

  [[nodiscard]] size_t size() const override { return m_blockOffsets.size(); }

  void finalize(std::optional<size_t> blockCount = {}) {
    stopThreads();
    m_rawBlockFinder = {};
    m_blockOffsets.finalize(blockCount);
  }

  [[nodiscard]] bool finalized() const override {
    return m_blockOffsets.finalized();
  }

  using BlockFinderInterface::get;

  [[nodiscard]] std::pair<std::optional<size_t>, GetReturnCode>
  get(size_t blockNumber, double timeoutInSeconds) override {
#ifdef WITH_PYTHON_SUPPORT
    const ScopedGILUnlock unlockedGIL;
#endif

    if (!m_blockOffsets.finalized()) {
      startThreads();
    }

    {
      const std::scoped_lock lock(m_mutex);
      m_highestRequestedBlockNumber =
          std::max(m_highestRequestedBlockNumber, blockNumber);
      m_changed.notify_all();
    }

    return m_blockOffsets.get(blockNumber, timeoutInSeconds);
  }

  [[nodiscard]] size_t find(size_t encodedBlockOffsetInBits) const override {
    const std::scoped_lock lock(m_mutex);

    const auto lockedOffsets = m_blockOffsets.results();
    const auto &blockOffsets = lockedOffsets.results();

    const auto match = std::lower_bound(
        blockOffsets.begin(), blockOffsets.end(), encodedBlockOffsetInBits);
    if ((match == blockOffsets.end()) || (*match != encodedBlockOffsetInBits)) {
      throw std::out_of_range("No block with the specified offset exists in "
                              "the gzip block finder map!");
    }

    return std::distance(blockOffsets.begin(), match);
  }

  void setBlockOffsets(BlockOffsets blockOffsets) {

    stopThreads();
    m_rawBlockFinder = {};

    m_blockOffsets.setResults(std::move(blockOffsets));
  }

private:
  void blockFinderMain() {
    while (!m_cancelThread) {
      std::unique_lock lock(m_mutex);

      m_changed.wait(lock, [this] {
        return m_cancelThread ||
               (m_blockOffsets.size() <=
                m_highestRequestedBlockNumber + m_prefetchCount);
      });
      if (m_cancelThread) {
        break;
      }

      lock.unlock();

      const auto blockOffset = m_rawBlockFinder->find();
      if (blockOffset == std::numeric_limits<size_t>::max()) {
        break;
      }

      lock.lock();
      m_blockOffsets.push(blockOffset);
    }

    m_blockOffsets.finalize();
  }

private:
  mutable std::mutex m_mutex;

  std::condition_variable m_changed;

  StreamedResults<size_t> m_blockOffsets;

  size_t m_highestRequestedBlockNumber{0};

  const size_t m_prefetchCount = 3ULL * std::thread::hardware_concurrency();

  std::unique_ptr<RawBlockFinder> m_rawBlockFinder;
  std::atomic<bool> m_cancelThread{false};

  std::unique_ptr<JoiningThread> m_blockFinder;
};
} // namespace rapidgzip
