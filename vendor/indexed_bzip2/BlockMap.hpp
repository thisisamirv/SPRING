#pragma once

#include <algorithm>
#include <cassert>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <common.hpp>

namespace rapidgzip {

class BlockMap {
public:
  struct BlockInfo {
  public:
    [[nodiscard]] bool contains(size_t dataOffset) const {
      return (decodedOffsetInBytes <= dataOffset) &&
             (dataOffset < decodedOffsetInBytes + decodedSizeInBytes);
    }

    friend std::ostream &operator<<(std::ostream &out,
                                    const BlockInfo &blockInfo) {
      out << "BlockInfo{ blockIndex: " << blockInfo.blockIndex
          << ", encodedOffsetInBits: "
          << formatBits(blockInfo.encodedOffsetInBits)
          << ", encodedSizeInBits: " << formatBits(blockInfo.encodedSizeInBits)
          << ", decodedOffsetInBytes: "
          << formatBytes(blockInfo.decodedOffsetInBytes)
          << ", decodedSizeInBytes: "
          << formatBytes(blockInfo.decodedSizeInBytes) << " }";
      return out;
    };

  public:
    size_t blockIndex{0};
    size_t encodedOffsetInBits{0};
    size_t encodedSizeInBits{0};
    size_t decodedOffsetInBytes{0};
    size_t decodedSizeInBytes{0};
  };

  using BlockOffsets = std::vector<std::pair<size_t, size_t>>;

public:
  BlockMap() = default;

  size_t push(size_t encodedBlockOffset, size_t encodedSize,
              size_t decodedSize) {
    const std::scoped_lock lock(m_mutex);

    if (m_finalized) {
      throw std::invalid_argument("May not insert into finalized block map!");
    }

    std::optional<size_t> decodedOffset;
    if (m_blockToDataOffsets.empty()) {
      decodedOffset = 0;
    } else if (encodedBlockOffset > m_blockToDataOffsets.back().first) {
      decodedOffset =
          m_blockToDataOffsets.back().second + m_lastBlockDecodedSize;
    }

    if (decodedOffset) {
      m_blockToDataOffsets.emplace_back(encodedBlockOffset, *decodedOffset);
      if (decodedSize == 0) {
        m_eosBlocks.emplace_back(encodedBlockOffset);
      }
      m_lastBlockDecodedSize = decodedSize;
      m_lastBlockEncodedSize = encodedSize;
      return *decodedOffset;
    }

    const auto match = std::lower_bound(
        m_blockToDataOffsets.begin(), m_blockToDataOffsets.end(),
        std::make_pair(encodedBlockOffset, 0),
        [](const auto &a, const auto &b) { return a.first < b.first; });

    if ((match == m_blockToDataOffsets.end()) ||
        (match->first != encodedBlockOffset)) {
      throw std::invalid_argument(
          "Inserted block offsets should be strictly increasing!");
    }

    if (std::next(match) == m_blockToDataOffsets.end()) {
      throw std::logic_error("In this case, the new block should already have "
                             "been appended above!");
    }

    const auto impliedDecodedSize = std::next(match)->second - match->second;
    if (impliedDecodedSize != decodedSize) {
      throw std::invalid_argument(
          "Got duplicate block offset with inconsistent size!");
    }

    return match->second;
  }

  [[nodiscard]] BlockInfo findDataOffset(size_t dataOffset) const {
    const std::scoped_lock lock(m_mutex);

    const auto blockOffset = std::lower_bound(
        m_blockToDataOffsets.rbegin(), m_blockToDataOffsets.rend(),
        std::make_pair(0, dataOffset),
        [](std::pair<size_t, size_t> a, std::pair<size_t, size_t> b) {
          return a.second > b.second;
        });

    if (blockOffset == m_blockToDataOffsets.rend()) {
      return {};
    }

    if (dataOffset < blockOffset->second) {
      throw std::logic_error(
          "Algorithm for finding the block to an offset is faulty!");
    }

    return get(blockOffset);
  }

  [[nodiscard]] std::optional<BlockInfo>
  getEncodedOffset(size_t encodedOffsetInBits) const {
    const std::scoped_lock lock(m_mutex);

    const auto blockOffset = std::lower_bound(
        m_blockToDataOffsets.rbegin(), m_blockToDataOffsets.rend(),
        std::make_pair(encodedOffsetInBits, 0),
        [](std::pair<size_t, size_t> a, std::pair<size_t, size_t> b) {
          return a.first > b.first;
        });

    if ((blockOffset == m_blockToDataOffsets.rend()) ||
        (blockOffset->first != encodedOffsetInBits)) {
      return std::nullopt;
    }

    return get(blockOffset);
  }

  [[nodiscard]] size_t dataBlockCount() const {
    const std::scoped_lock lock(m_mutex);
    return m_blockToDataOffsets.size() - m_eosBlocks.size();
  }

  void finalize() {
    const std::scoped_lock lock(m_mutex);

    if (m_finalized) {
      return;
    }

    if (m_blockToDataOffsets.empty()) {
      assert(m_lastBlockEncodedSize == 0);
      assert(m_lastBlockDecodedSize == 0);
      m_blockToDataOffsets.emplace_back(m_lastBlockEncodedSize,
                                        m_lastBlockDecodedSize);
    } else if ((m_lastBlockEncodedSize != 0) || (m_lastBlockDecodedSize != 0)) {
      const auto &[lastEncodedOffset, lastDecodedOffset] =
          m_blockToDataOffsets.back();
      m_blockToDataOffsets.emplace_back(
          lastEncodedOffset + m_lastBlockEncodedSize,
          lastDecodedOffset + m_lastBlockDecodedSize);
    }

    m_lastBlockEncodedSize = 0;
    m_lastBlockDecodedSize = 0;

    m_finalized = true;
  }

  [[nodiscard]] bool finalized() const {
    const std::scoped_lock lock(m_mutex);
    return m_finalized;
  }

  void setBlockOffsets(std::map<size_t, size_t> const &blockOffsets) {
    const std::scoped_lock lock(m_mutex);

    m_blockToDataOffsets.assign(blockOffsets.begin(), blockOffsets.end());
    m_lastBlockEncodedSize = 0;
    m_lastBlockDecodedSize = 0;

    m_eosBlocks.clear();
    for (auto it = m_blockToDataOffsets.begin(),
              nit = std::next(m_blockToDataOffsets.begin());
         nit != m_blockToDataOffsets.end(); ++it, ++nit) {

      if (it->second == nit->second) {
        m_eosBlocks.push_back(it->first);
      }
    }

    m_eosBlocks.push_back(m_blockToDataOffsets.back().first);

    m_finalized = true;
  }

  [[nodiscard]] std::map<size_t, size_t> blockOffsets() const {
    const std::scoped_lock lock(m_mutex);

    return {m_blockToDataOffsets.begin(), m_blockToDataOffsets.end()};
  }

  [[nodiscard]] std::pair<size_t, size_t> back() const {
    const std::scoped_lock lock(m_mutex);

    if (m_blockToDataOffsets.empty()) {
      throw std::out_of_range(
          "Can not return last element of empty block map!");
    }
    return m_blockToDataOffsets.back();
  }

  [[nodiscard]] bool empty() const { return m_blockToDataOffsets.empty(); }

private:
  [[nodiscard]] BlockInfo
  get(const typename BlockOffsets::const_reverse_iterator &blockOffset) const {
    BlockInfo result;
    if (blockOffset == m_blockToDataOffsets.rend()) {
      return result;
    }

    result.encodedOffsetInBits = blockOffset->first;
    result.decodedOffsetInBytes = blockOffset->second;
    result.blockIndex =
        std::distance(blockOffset, m_blockToDataOffsets.rend()) - 1;

    if (blockOffset == m_blockToDataOffsets.rbegin()) {
      result.decodedSizeInBytes = m_lastBlockDecodedSize;
      result.encodedSizeInBits = m_lastBlockEncodedSize;
    } else {
      const auto higherBlock = std::prev(blockOffset);
      if (higherBlock->second < blockOffset->second) {
        throw std::logic_error(
            "Data offsets are not monotonically increasing!");
      }
      result.decodedSizeInBytes = higherBlock->second - blockOffset->second;
      result.encodedSizeInBits = higherBlock->first - blockOffset->first;
    }

    return result;
  }

private:
  mutable std::mutex m_mutex;

  BlockOffsets m_blockToDataOffsets;
  std::vector<size_t> m_eosBlocks;
  bool m_finalized{false};

  size_t m_lastBlockEncodedSize{0};
  size_t m_lastBlockDecodedSize{0};
};
} // namespace rapidgzip
