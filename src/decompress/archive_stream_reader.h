// Declares the SpringReader streaming decompression API used by library-style
// consumers of SPRING2 archives.

#ifndef SPRING_ARCHIVE_STREAM_READER_H_
#define SPRING_ARCHIVE_STREAM_READER_H_

#include "archive_record_reconstruction.h"
#include <memory>
#include <string>

namespace spring {

/**
 * @brief High-level streaming interface for decompressing SPRING2 archives.
 *
 * This class provides a library-style "pull" API. It handles archive
 * extraction, temporary resource management, and leverages background
 * pre-fetching to maintain high throughout without stalling the caller.
 */
class SpringReader {
public:
  /**
   * @brief Opens a SPRING2 archive for streaming.
   *
   * @param archive_path Path to the .sp archive.
   * @param num_thr Number of threads to use for background decompression (0 for
   * auto).
   */
  explicit SpringReader(const std::string &archive_path, int num_thr = 0);

  ~SpringReader();

  // Prevent copying to manage background thread safety.
  SpringReader(const SpringReader &) = delete;
  SpringReader &operator=(const SpringReader &) = delete;

  /**
   * @brief Access the compression parameters and metadata stored in the
   * archive.
   */
  const compression_params &params() const;

  /**
   * @brief Fetches the next record for single-end archives.
   *
   * @param record Output struct to populate.
   * @return true if a record was successfully read, false if EOF is reached.
   * @throws std::runtime_error on paired-end archives (use the paired-end
   * overload).
   */
  bool next(ReadRecord &record);

  /**
   * @brief Fetches the next pair of records for paired-end archives.
   *
   * @param mate1 Output struct for the first mate.
   * @param mate2 Output struct for the second mate.
   * @return true if a pair was successfully read, false if EOF is reached.
   * @throws std::runtime_error on single-end archives (use the single-end
   * overload).
   */
  bool next(ReadRecord &mate1, ReadRecord &mate2);

  /**
   * @brief Retrieves the computed integrity digests for the archive.
   */
  void get_digests(uint32_t seq_crc[2], uint32_t qual_crc[2],
                   uint32_t id_crc[2]);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace spring

#endif // SPRING_ARCHIVE_STREAM_READER_H_
