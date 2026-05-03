#pragma once

#include <algorithm>
#include <cassert>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <BlockFinderInterface.hpp>
#include <common.hpp>

#include "Bgzf.hpp"
#include "format.hpp"

namespace rapidgzip {

class GzipBlockFinder final : public BlockFinderInterface {
public:
  using BlockOffsets = std::vector<size_t>;

public:
  explicit GzipBlockFinder(UniqueFileReader fileReader, size_t spacing)
      : m_file(std::move(fileReader)),
        m_fileSizeInBits(m_file->size()
                             ? std::make_optional(*m_file->size() * CHAR_BIT)
                             : std::nullopt),
        m_spacingInBits(spacing * CHAR_BIT) {
    if (m_spacingInBits < 32_Ki) {

      throw std::invalid_argument(
          "A spacing smaller than the window size makes no sense!");
    }

    const auto detectedFormat = determineFileTypeAndOffset(m_file);
    if (!detectedFormat) {
      throw std::invalid_argument("Failed to detect a valid file format.");
    }

    m_fileType = detectedFormat->first;
    if (m_fileType == FileType::BGZF) {
      m_bgzfBlockFinder = std::make_unique<blockfinder::Bgzf>(m_file->clone());
    }

    m_blockOffsets.push_back(detectedFormat->second);
  }

  [[nodiscard]] size_t size() const override {
    const std::scoped_lock lock(m_mutex);
    return m_blockOffsets.size();
  }

  void finalize() {
    const std::scoped_lock lock(m_mutex);
    m_finalized = true;
  }

  [[nodiscard]] bool finalized() const override {
    const std::scoped_lock lock(m_mutex);
    return m_finalized;
  }

  [[nodiscard]] FileType fileType() const noexcept { return m_fileType; }

  void insert(size_t blockOffset) {
    const std::scoped_lock lock(m_mutex);
    insertUnsafe(blockOffset);
  }

  using BlockFinderInterface::get;

  [[nodiscard]] std::pair<std::optional<size_t>, GetReturnCode>
  get(size_t blockIndex, [[maybe_unused]] double timeoutInSeconds) override {
    const std::scoped_lock lock(m_mutex);

    if (m_fileType == FileType::BGZF) {
      return getBgzfBlock(blockIndex);
    }

    if (blockIndex < m_blockOffsets.size()) {
      return {m_blockOffsets[blockIndex], GetReturnCode::SUCCESS};
    }

    assert(!m_blockOffsets.empty());
    const auto blockIndexOutside = blockIndex - m_blockOffsets.size();
    const auto partitionIndex = firstPartitionIndex() + blockIndexOutside;
    const auto blockOffset = partitionIndex * m_spacingInBits;

    if (!m_fileSizeInBits) {
      if (const auto fileSize = m_file->size()) {
        m_fileSizeInBits = *fileSize * CHAR_BIT;
      }
    }
    if (!m_fileSizeInBits || (blockOffset < *m_fileSizeInBits)) {
      return {blockOffset, GetReturnCode::SUCCESS};
    }

    if (partitionIndex > 0) {
      return {*m_fileSizeInBits, GetReturnCode::FAILURE};
    }

    return {0, GetReturnCode::FAILURE};
  }

  [[nodiscard]] size_t find(size_t encodedBlockOffsetInBits) const override {
    const std::scoped_lock lock(m_mutex);

    const auto match = std::lower_bound(
        m_blockOffsets.begin(), m_blockOffsets.end(), encodedBlockOffsetInBits);
    if ((match != m_blockOffsets.end()) &&
        (*match == encodedBlockOffsetInBits)) {
      return std::distance(m_blockOffsets.begin(), match);
    }

    if ((encodedBlockOffsetInBits > m_blockOffsets.back()) &&
        (encodedBlockOffsetInBits % m_spacingInBits == 0)) {
      const auto blockIndex =
          m_blockOffsets.size() +
          (encodedBlockOffsetInBits / m_spacingInBits - firstPartitionIndex());
      assert((firstPartitionIndex() + (blockIndex - m_blockOffsets.size())) *
                 m_spacingInBits ==
             encodedBlockOffsetInBits);
      return blockIndex;
    }

    throw std::out_of_range("No block with the specified offset " +
                            std::to_string(encodedBlockOffsetInBits) +
                            " exists in the block finder map!");
  }

  void setBlockOffsets(const std::vector<size_t> &blockOffsets) {
    m_blockOffsets.assign(blockOffsets.begin(), blockOffsets.end());
    finalize();
  }

  [[nodiscard]] size_t
  partitionOffsetContainingOffset(size_t blockOffset) const {

    return (blockOffset / m_spacingInBits) * m_spacingInBits;
  }

  [[nodiscard]] constexpr size_t spacingInBits() const noexcept {
    return m_spacingInBits;
  }

private:
  [[nodiscard]] std::optional<size_t> fileSize() {
    if (m_fileSizeInBits) {
      return m_fileSizeInBits;
    }

    const auto fileSize = m_file->size();
    if (fileSize) {
      m_fileSizeInBits = *fileSize * CHAR_BIT;
      return m_fileSizeInBits;
    }

    return std::nullopt;
  }

  bool insertUnsafe(size_t blockOffset) {
    const auto size = fileSize();
    if (size.has_value() && (blockOffset >= *size)) {
      return false;
    }

    const auto match = std::lower_bound(m_blockOffsets.begin(),
                                        m_blockOffsets.end(), blockOffset);
    if ((match == m_blockOffsets.end()) || (*match != blockOffset)) {
      if (m_finalized) {
        throw std::invalid_argument(
            "Already finalized, may not insert further block offsets!");
      }
      m_blockOffsets.insert(match, blockOffset);
      assert(std::is_sorted(m_blockOffsets.begin(), m_blockOffsets.end()));
    }

    return true;
  }

  void gatherMoreBgzfBlocks(size_t blockIndex) {
    while (blockIndex + m_batchFetchCount >= m_blockOffsets.size()) {
      const auto nextOffset = m_bgzfBlockFinder->find();
      if (nextOffset < m_blockOffsets.back() + m_spacingInBits) {
        continue;
      }
      if (!insertUnsafe(nextOffset)) {
        break;
      }
    }
  }

  [[nodiscard]] std::pair<std::optional<size_t>, GetReturnCode>
  getBgzfBlock(size_t blockIndex) {
    if (m_bgzfBlockFinder && !m_finalized) {
      gatherMoreBgzfBlocks(blockIndex);
    }

    if (blockIndex < m_blockOffsets.size()) {
      return {m_blockOffsets[blockIndex], GetReturnCode::SUCCESS};
    }

    return {fileSize().value_or(std::numeric_limits<size_t>::max()),
            GetReturnCode::FAILURE};
  }

  [[nodiscard]] size_t firstPartitionIndex() const {

    return m_blockOffsets.back() / m_spacingInBits + 1;
  }

private:
  mutable std::mutex m_mutex;

  const UniqueFileReader m_file;
  std::optional<size_t> m_fileSizeInBits;
  bool m_finalized{false};
  const size_t m_spacingInBits;

  std::deque<size_t> m_blockOffsets;

  FileType m_fileType{FileType::NONE};
  std::unique_ptr<blockfinder::Bgzf> m_bgzfBlockFinder;
  const size_t m_batchFetchCount =
      std::max<size_t>(16, 3ULL * std::thread::hardware_concurrency());
};
} // namespace rapidgzip
