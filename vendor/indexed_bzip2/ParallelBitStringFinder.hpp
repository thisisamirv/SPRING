#pragma once

#include <climits>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <list>
#include <mutex>
#include <queue>
#include <utility>

#include "AffinityHelpers.hpp"
#include "BitStringFinder.hpp"
#include "ThreadPool.hpp"
#include "common.hpp"

namespace rapidgzip {

template <uint8_t bitStringSize>
class ParallelBitStringFinder : public BitStringFinder<bitStringSize> {
public:
  using BaseType = BitStringFinder<bitStringSize>;

  static_assert(bitStringSize > 0,
                "Bit string to find must have positive length!");

public:
  ParallelBitStringFinder(
      UniqueFileReader fileReader, uint64_t bitStringToFind,
      size_t parallelization = std::max(1U, availableCores() / 8U),
      size_t requestedBytes = 0, size_t fileBufferSizeBytes = 1_Mi)
      : BaseType(
            std::move(fileReader), bitStringToFind,
            chunkSize(fileBufferSizeBytes, requestedBytes, parallelization)),
        m_threadPool(parallelization) {}

  ParallelBitStringFinder(const char *buffer, size_t size,
                          uint64_t bitStringToFind, size_t parallelization)
      : BaseType(UniqueFileReader(), bitStringToFind),
        m_threadPool(parallelization) {
    this->m_buffer.assign(buffer, buffer + size);
  }

  ~ParallelBitStringFinder() override = default;

  [[nodiscard]] size_t find() override;

private:
  struct ThreadResults {
    std::queue<size_t> foundOffsets;
    std::mutex mutex;
    std::future<void> future;
    std::condition_variable changed;
  };

private:
  [[nodiscard]] static constexpr size_t
  chunkSize(size_t const fileBufferSizeBytes, size_t const requestedBytes,
            size_t const parallelization) {

    const auto result = std::max(
        fileBufferSizeBytes,
        static_cast<size_t>(ceilDiv(bitStringSize, 8)) * parallelization);

    return std::max(result, requestedBytes);
  }

  static void
  workerMain(char const *const buffer, size_t const bufferSizeInBytes,
             uint8_t const firstBitsToIgnore, uint64_t const bitStringToFind,
             size_t const bitOffsetToAdd, ThreadResults *const result) {
    auto offsets =
        BaseType::findBitStrings({buffer, bufferSizeInBytes}, bitStringToFind);
    std::sort(offsets.begin(), offsets.end());

    const std::lock_guard<std::mutex> lock(result->mutex);
    for (const auto offset : offsets) {
      if (offset >= firstBitsToIgnore) {
        result->foundOffsets.push(bitOffsetToAdd + offset);
      }
    }
    result->foundOffsets.push(std::numeric_limits<size_t>::max());
    result->changed.notify_one();
  }

private:
  const size_t m_requestedBytes = 0;

  std::list<ThreadResults> m_threadResults;

  ThreadPool m_threadPool;
};

template <uint8_t bitStringSize>
size_t ParallelBitStringFinder<bitStringSize>::find() {
#ifdef WITH_PYTHON_SUPPORT
  const ScopedGILUnlock unlockedGIL;
#endif

  while (!BaseType::eof() || !m_threadResults.empty()) {

    while (!m_threadResults.empty()) {
      auto &result = m_threadResults.front();
      using namespace std::chrono;

      std::unique_lock<std::mutex> lock(result.mutex);
      while (!result.foundOffsets.empty() || result.future.valid()) {

        if (!result.foundOffsets.empty()) {
          if (result.foundOffsets.front() ==
              std::numeric_limits<size_t>::max()) {
            result.foundOffsets.pop();
            if (result.future.valid()) {
              result.future.get();
            }
            break;
          }
          const auto foundOffset = result.foundOffsets.front();
          result.foundOffsets.pop();
          return foundOffset;
        }

        result.changed.wait(lock, [&result]() {
          return !result.foundOffsets.empty() ||
                 (result.future.wait_for(0s) == std::future_status::ready);
        });

        if (result.future.wait_for(0s) == std::future_status::ready) {
          result.future.get();
        }
      }
      lock = {};

      if (result.future.valid() || !result.foundOffsets.empty()) {
        throw std::logic_error(
            "Should have gotten future and emptied offsets!");
      }
      m_threadResults.pop_front();
    }

    if (this->bufferEof()) {
      const auto nBytesRead = BaseType::refillBuffer();
      if (nBytesRead == 0) {
        return std::numeric_limits<size_t>::max();
      }
    }

    const auto minSubChunkSizeInBytes =
        std::max<size_t>(8UL * bitStringSize, 4096);
    const auto subChunkStrideInBytes = std::max<size_t>(
        minSubChunkSizeInBytes,
        ceilDiv(this->m_buffer.size(), m_threadPool.capacity()));

    for (; !this->bufferEof();
         this->m_bufferBitsRead += subChunkStrideInBytes * CHAR_BIT) {

      auto const bufferOffsetInBits =
          this->m_bufferBitsRead > this->m_movingBitsToKeep
              ? this->m_bufferBitsRead - this->m_movingBitsToKeep
              : this->m_bufferBitsRead;
      auto const bufferOffsetInBytes = bufferOffsetInBits / CHAR_BIT;
      auto const subChunkOffsetInBits =
          static_cast<uint8_t>(bufferOffsetInBits % CHAR_BIT);

      auto const subChunkSizeInBits = this->m_bufferBitsRead -
                                      bufferOffsetInBits +
                                      subChunkStrideInBytes * CHAR_BIT;
      auto const subChunkSizeInBytes =
          std::min(ceilDiv(subChunkSizeInBits, CHAR_BIT),
                   this->m_buffer.size() - bufferOffsetInBytes);

      auto &result = m_threadResults.emplace_back();
      result.future = m_threadPool.submit([=, &result]() {
        workerMain(this->m_buffer.data() + bufferOffsetInBytes,
                   subChunkSizeInBytes, subChunkOffsetInBits,
                   this->m_bitStringToFind,
                   (this->m_nTotalBytesRead + bufferOffsetInBytes) * CHAR_BIT,
                   &result);
      });
    }
  }

  return std::numeric_limits<size_t>::max();
}
} // namespace rapidgzip
