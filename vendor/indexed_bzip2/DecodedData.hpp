#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

#include <FasterVector.hpp>
#include <VectorView.hpp>

#include "DecodedDataView.hpp"
#include "MarkerReplacement.hpp"
#include "definitions.hpp"

namespace rapidgzip::deflate {
using MarkerVector = FasterVector<uint16_t>;
using DecodedVector = FasterVector<uint8_t>;

struct DecodedData {
public:
  using WindowView = VectorView<uint8_t>;

  class Iterator {
  public:
    explicit Iterator(const DecodedData &decodedData,
                      const size_t offsetInChunk = 0,
                      const size_t size = std::numeric_limits<size_t>::max())
        : m_data(decodedData), m_size(size), m_offsetInChunk(offsetInChunk) {

      for (m_currentChunk = 0; m_currentChunk < m_data.data.size();
           ++m_currentChunk) {
        const auto &chunk = m_data.data[m_currentChunk];
        if ((m_offsetInChunk < chunk.size()) && !chunk.empty()) {
          m_sizeInChunk = std::min(chunk.size() - m_offsetInChunk, m_size);
          break;
        }
        m_offsetInChunk -= chunk.size();
      }
    }

    [[nodiscard]] operator bool() const noexcept {
      return (m_currentChunk < m_data.data.size()) &&
             (m_processedSize < m_size);
    }

    [[nodiscard]] std::pair<const void *, size_t> operator*() const {
      const auto &chunk = m_data.data[m_currentChunk];
      return {chunk.data() + m_offsetInChunk, m_sizeInChunk};
    }

    void operator++() {
      m_processedSize += m_sizeInChunk;
      m_offsetInChunk = 0;
      m_sizeInChunk = 0;

      if (m_processedSize > m_size) {
        throw std::logic_error("Iterated over more bytes than was requested!");
      }

      if (!static_cast<bool>(*this)) {
        return;
      }

      ++m_currentChunk;
      for (; m_currentChunk < m_data.data.size(); ++m_currentChunk) {
        const auto &chunk = m_data.data[m_currentChunk];
        if (!chunk.empty()) {
          m_sizeInChunk = std::min(chunk.size(), m_size - m_processedSize);
          break;
        }
      }
    }

  private:
    const DecodedData &m_data;
    const size_t m_size;

    size_t m_currentChunk{0};
    size_t m_offsetInChunk{0};
    size_t m_sizeInChunk{0};
    size_t m_processedSize{0};
  };

public:
  void append(DecodedVector &&toAppend) {
    if (!toAppend.empty()) {
      dataBuffers.emplace_back(std::move(toAppend));
      dataBuffers.back().shrink_to_fit();
      data.emplace_back(dataBuffers.back().data(), dataBuffers.back().size());
    }
  }

  void append(DecodedDataView const &buffers);

  [[nodiscard]] size_t dataSize() const noexcept {
    const auto addSize = [](const size_t size, const auto &container) {
      return size + container.size();
    };
    return std::accumulate(data.begin(), data.end(), size_t(0), addSize);
  }

  [[nodiscard]] size_t dataWithMarkersSize() const noexcept {
    const auto addSize = [](const size_t size, const auto &container) {
      return size + container.size();
    };
    return std::accumulate(dataWithMarkers.begin(), dataWithMarkers.end(),
                           size_t(0), addSize);
  }

  [[nodiscard]] size_t size() const noexcept {
    return dataSize() + dataWithMarkersSize();
  }

  [[nodiscard]] size_t sizeInBytes() const noexcept {
    return dataSize() * sizeof(uint8_t) +
           dataWithMarkersSize() * sizeof(uint16_t);
  }

  [[nodiscard]] bool containsMarkers() const noexcept {
    return !dataWithMarkers.empty();
  }

  [[nodiscard]] size_t countMarkerSymbols() const;

  void applyWindow(WindowView const &window);

  [[nodiscard]] DecodedVector
  getLastWindow(WindowView const &previousWindow) const;

  [[nodiscard]] DecodedVector getWindowAt(WindowView const &previousWindow,
                                          size_t skipBytes) const;

  void shrinkToFit() {
    for (auto &container : dataBuffers) {
      container.shrink_to_fit();
    }
    for (auto &container : dataWithMarkers) {
      container.shrink_to_fit();
    }
  }

  void cleanUnmarkedData();

#ifdef TEST_DECODED_DATA
  [[nodiscard]] const std::vector<MarkerVector> &
  getDataWithMarkers() const noexcept {
    return dataWithMarkers;
  }

  [[nodiscard]] const std::vector<VectorView<uint8_t>> &
  getData() const noexcept {
    return data;
  }
#endif

private:
  std::vector<MarkerVector> dataWithMarkers;
  std::vector<MarkerVector> reusedDataBuffers;
  std::vector<DecodedVector> dataBuffers;
  std::vector<VectorView<uint8_t>> data;
};

inline void DecodedData::append(DecodedDataView const &buffers) {
  static constexpr auto ALLOCATION_CHUNK_SIZE = 128_Ki;

  const auto &appendToEquallySizedChunks = [](auto &targetChunks,
                                              const auto &buffer) {
    constexpr auto ALLOCATION_ELEMENT_COUNT =
        ALLOCATION_CHUNK_SIZE / sizeof(targetChunks[0][0]);

    if (targetChunks.empty()) {
      targetChunks.emplace_back().reserve(ALLOCATION_ELEMENT_COUNT);
    }

    for (size_t nCopied = 0; nCopied < buffer.size();) {
      auto &copyTarget = targetChunks.back();
      const auto nFreeElements = copyTarget.capacity() - copyTarget.size();
      if (nFreeElements == 0) {
        targetChunks.emplace_back().reserve(ALLOCATION_ELEMENT_COUNT);
        continue;
      }

      const auto nToCopy = std::min(nFreeElements, buffer.size() - nCopied);
      copyTarget.insert(copyTarget.end(), buffer.begin() + nCopied,
                        buffer.begin() + nCopied + nToCopy);
      nCopied += nToCopy;
    }
  };

  if (buffers.dataWithMarkersSize() > 0) {
    if (!data.empty()) {
      throw std::invalid_argument(
          "It is not allowed to append data with markers when fully decoded "
          "data "
          "has already been appended because the ordering will be wrong!");
    }

    for (const auto &buffer : buffers.dataWithMarkers) {
      appendToEquallySizedChunks(dataWithMarkers, buffer);
    }
  }

  if (buffers.dataSize() > 0) {
    auto &copied = dataBuffers.emplace_back();
    copied.reserve(buffers.dataSize());
    for (const auto &buffer : buffers.data) {
      copied.insert(copied.end(), buffer.begin(), buffer.end());
    }
    data.emplace_back(copied.data(), copied.size());
  }
}

[[nodiscard]] inline size_t DecodedData::countMarkerSymbols() const {
  size_t result{0};
  for (const auto &chunk : dataWithMarkers) {
    result +=
        std::count_if(chunk.begin(), chunk.end(), [](const uint16_t symbol) {
          return (symbol & 0xFF00U) != 0;
        });
  }
  return result;
}

inline void DecodedData::applyWindow(WindowView const &window) {
  const auto markerCount = dataWithMarkersSize();
  if (markerCount == 0) {
    dataWithMarkers.clear();
    return;
  }

  if (markerCount >= 128_Ki) {
    const std::array<uint8_t, 64_Ki> fullWindow = [&window]() noexcept {
      std::array<uint8_t, 64_Ki> result{};
      std::iota(result.begin(), result.begin() + 256, 0);
      std::copy(window.begin(), window.end(), result.begin() + MAX_WINDOW_SIZE);
      return result;
    }();

    for (auto &chunk : dataWithMarkers) {
      auto *const target = reinterpret_cast<uint8_t *>(chunk.data());

      for (size_t i = 0; i < chunk.size(); ++i) {
        target[i] = fullWindow[chunk[i]];
      }
    }
  } else {

    static_assert(std::numeric_limits<uint16_t>::max() - MAX_WINDOW_SIZE + 1U ==
                  MAX_WINDOW_SIZE);
    if (window.size() >= MAX_WINDOW_SIZE) {
      const MapMarkers<true> mapMarkers(window);
      for (auto &chunk : dataWithMarkers) {

        auto *const target = reinterpret_cast<uint8_t *>(chunk.data());
        for (size_t i = 0; i < chunk.size(); ++i) {
          target[i] = mapMarkers(chunk[i]);
        }
      }
    } else {
      const MapMarkers<false> mapMarkers(window);
      for (auto &chunk : dataWithMarkers) {
        auto *const target = reinterpret_cast<uint8_t *>(chunk.data());

        for (size_t i = 0; i < chunk.size(); ++i) {
          target[i] = mapMarkers(chunk[i]);
        }
      }
    }
  }

  if (!reusedDataBuffers.empty()) {
    throw std::logic_error(
        "It seems like data already was replaced but we still got markers!");
  }
  std::swap(reusedDataBuffers, dataWithMarkers);

  std::vector<VectorView<uint8_t>> dataViews;
  dataViews.reserve(reusedDataBuffers.size() + data.size());
  for (auto &chunk : reusedDataBuffers) {

    dataViews.emplace_back(reinterpret_cast<uint8_t *>(chunk.data()),
                           chunk.size());
  }
  std::move(data.begin(), data.end(), std::back_inserter(dataViews));
  std::swap(data, dataViews);

  dataWithMarkers.clear();
}

[[nodiscard]] inline DecodedVector
DecodedData::getLastWindow(WindowView const &previousWindow) const {
  return getWindowAt(previousWindow, size());
}

[[nodiscard]] inline DecodedVector
DecodedData::getWindowAt(WindowView const &previousWindow,
                         size_t const skipBytes) const {
  if (skipBytes > size()) {
    throw std::invalid_argument(
        "Amount of bytes to skip is larger than this block!");
  }

  DecodedVector window(MAX_WINDOW_SIZE);
  size_t prefilled{0};
  if (skipBytes < MAX_WINDOW_SIZE) {
    const auto lastBytesToCopyFromPrevious = MAX_WINDOW_SIZE - skipBytes;
    if (lastBytesToCopyFromPrevious <= previousWindow.size()) {
      for (size_t j = previousWindow.size() - lastBytesToCopyFromPrevious;
           j < previousWindow.size(); ++j, ++prefilled) {
        window[prefilled] = previousWindow[j];
      }

    } else {

      const auto zerosToFill =
          lastBytesToCopyFromPrevious - previousWindow.size();
      for (; prefilled < zerosToFill; ++prefilled) {
        window[prefilled] = 0;
      }

      for (size_t j = 0; j < previousWindow.size(); ++j, ++prefilled) {
        window[prefilled] = previousWindow[j];
      }
    }
    assert(prefilled == MAX_WINDOW_SIZE - skipBytes);
  }

  const auto remainingBytes = window.size() - prefilled;

  auto offset = skipBytes - remainingBytes;

  const auto copyFromDataWithMarkers = [this, &offset, &prefilled,
                                        &window](const auto &mapMarker) {
    for (const auto &chunk : dataWithMarkers) {
      if (prefilled >= window.size()) {
        break;
      }

      if (offset >= chunk.size()) {
        offset -= chunk.size();
        continue;
      }

      for (size_t i = offset; (i < chunk.size()) && (prefilled < window.size());
           ++i, ++prefilled) {
        window[prefilled] = mapMarker(chunk[i]);
      }
      offset = 0;
    }
  };

  if (previousWindow.size() >= MAX_WINDOW_SIZE) {
    copyFromDataWithMarkers(MapMarkers<true>(previousWindow));
  } else {
    copyFromDataWithMarkers(MapMarkers<false>(previousWindow));
  }

  for (const auto &chunk : data) {
    if (prefilled >= window.size()) {
      break;
    }

    if (offset >= chunk.size()) {
      offset -= chunk.size();
      continue;
    }

    for (size_t i = offset; (i < chunk.size()) && (prefilled < window.size());
         ++i, ++prefilled) {
      window[prefilled] = chunk[i];
    }
    offset = 0;
  }

  return window;
}

inline void DecodedData::cleanUnmarkedData() {
  while (!dataWithMarkers.empty()) {
    const auto &toDowncast = dataWithMarkers.back();

    const auto marker =
        std::find_if(toDowncast.rbegin(), toDowncast.rend(), [](auto value) {
          return value > std::numeric_limits<uint8_t>::max();
        });

    const auto sizeWithoutMarkers =
        static_cast<size_t>(std::distance(toDowncast.rbegin(), marker));
    auto downcasted =
        dataBuffers.emplace(dataBuffers.begin(), sizeWithoutMarkers);
    data.insert(data.begin(),
                VectorView<uint8_t>(downcasted->data(), downcasted->size()));
    std::transform(marker.base(), toDowncast.end(), downcasted->begin(),
                   [](auto symbol) { return static_cast<uint8_t>(symbol); });

    if (marker == toDowncast.rend()) {
      dataWithMarkers.pop_back();
    } else {
      dataWithMarkers.back().resize(dataWithMarkers.back().size() -
                                    sizeWithoutMarkers);
      break;
    }
  }

  shrinkToFit();
}

static_assert(!std::is_polymorphic_v<DecodedData>,
              "Simply making it polymorphic halves performance!");

#ifdef HAVE_IOVEC
[[nodiscard]] inline std::vector<::iovec>
toIoVec(const DecodedData &decodedData, const size_t offsetInBlock,
        const size_t dataToWriteSize) {
  std::vector<::iovec> buffersToWrite;
  for (auto it = rapidgzip::deflate::DecodedData::Iterator(
           decodedData, offsetInBlock, dataToWriteSize);
       static_cast<bool>(it); ++it) {
    const auto &[data, size] = *it;
    ::iovec buffer{};

    buffer.iov_base = const_cast<void *>(reinterpret_cast<const void *>(data));
    ;
    buffer.iov_len = size;
    buffersToWrite.emplace_back(buffer);
  }
  return buffersToWrite;
}
#endif
} // namespace rapidgzip::deflate
