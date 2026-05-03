#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

#include <CompressedVector.hpp>
#include <DecodedData.hpp>
#include <FasterVector.hpp>
#include <VectorView.hpp>

namespace rapidgzip {
class WindowMap {
public:
  using Window = CompressedVector<FasterVector<uint8_t>>;
  using WindowView = VectorView<std::uint8_t>;
  using SharedWindow = std::shared_ptr<const Window>;
  using Windows = std::map<size_t, SharedWindow>;

public:
  WindowMap() = default;
  ~WindowMap() = default;
  WindowMap(WindowMap &&) = delete;
  WindowMap &operator=(WindowMap &&) = delete;
  WindowMap &operator=(const WindowMap &) = delete;

  explicit WindowMap(const WindowMap &other)
      : m_windows(*other.data().second) {}

  void emplace(const size_t encodedBlockOffset, WindowView window,
               CompressionType compressionType) {
    emplaceShared(encodedBlockOffset,
                  std::make_shared<Window>(window, compressionType));
  }

  void emplaceShared(const size_t encodedBlockOffset,
                     SharedWindow sharedWindow) {
    if (!sharedWindow) {
      return;
    }

    const std::scoped_lock lock(m_mutex);

    if (m_windows.empty()) {
      m_windows.emplace(encodedBlockOffset, std::move(sharedWindow));
    } else if (m_windows.rbegin()->first < encodedBlockOffset) {

      m_windows.emplace_hint(m_windows.end(), encodedBlockOffset,
                             std::move(sharedWindow));
    } else {

      m_windows.insert_or_assign(m_windows.end(), encodedBlockOffset,
                                 std::move(sharedWindow));
    }
  }

  [[nodiscard]] SharedWindow get(size_t encodedOffsetInBits) const {

    const std::scoped_lock lock(m_mutex);
    if (const auto match = m_windows.find(encodedOffsetInBits);
        match != m_windows.end()) {
      return match->second;
    }
    return nullptr;
  }

  [[nodiscard]] bool empty() const {
    const std::scoped_lock lock(m_mutex);
    return m_windows.empty();
  }

  void releaseUpTo(size_t encodedOffset) {
    const std::scoped_lock lock(m_mutex);
    auto start = m_windows.begin();
    auto end = start;
    while ((end != m_windows.end()) && (end->first < encodedOffset)) {
      ++end;
    }
    m_windows.erase(start, end);
  }

  [[nodiscard]] std::pair<std::unique_lock<std::mutex>, Windows *> data() {
    return {std::unique_lock(m_mutex), &m_windows};
  }

  [[nodiscard]] std::pair<std::unique_lock<std::mutex>, const Windows *>
  data() const {
    return {std::unique_lock(m_mutex), &m_windows};
  }

  [[nodiscard]] size_t size() const {
    const std::scoped_lock lock(m_mutex);
    return m_windows.size();
  }

  [[nodiscard]] bool operator==(const WindowMap &other) const {
    const std::scoped_lock lock(m_mutex, other.m_mutex);

    if (m_windows.size() != other.m_windows.size()) {
      return false;
    }

    for (const auto &[offset, window] : m_windows) {
      const auto otherWindowIt = other.m_windows.find(offset);
      if ((otherWindowIt == other.m_windows.end()) ||
          (static_cast<bool>(window) !=
           static_cast<bool>(otherWindowIt->second))) {
        return false;
      }

      const auto &otherWindow = otherWindowIt->second;
      if (!static_cast<bool>(window) || !static_cast<bool>(otherWindow)) {
        continue;
      }

      if (*window != *otherWindow) {
        const auto a = window->decompress();
        const auto b = otherWindow->decompress();
        if ((static_cast<bool>(a) != static_cast<bool>(b)) ||
            (static_cast<bool>(a) && static_cast<bool>(b) && (*a != *b))) {
          return false;
        }
      }
    }

    return true;
  }

  [[nodiscard]] bool operator!=(const WindowMap &other) const {
    return !(*this == other);
  }

private:
  mutable std::mutex m_mutex;

  Windows m_windows;
};
} // namespace rapidgzip
