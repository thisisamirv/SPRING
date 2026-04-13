// Declares the short-read, long-read, and packed-sequence decompression entry
// points used by Spring's top-level archive restoration flow.

#ifndef SPRING_DECOMPRESS_H_
#define SPRING_DECOMPRESS_H_

#include <cstdint>
#include <fstream>
#include <string>

namespace spring {

struct compression_params;

/**
 * @brief Represents a single genomic record (from FASTA or FASTQ).
 */
struct ReadRecord {
  std::string id;       ///< Identifier line (excluding '@' or '>')
  std::string sequence; ///< DNA sequence
  std::string quality;  ///< Phred quality scores (empty for FASTA)
};

/**
 * @brief Abstract sink for receiving decompressed records.
 */
class DecompressionSink {
public:
  virtual ~DecompressionSink() = default;

  /**
   * @brief Called when a batch of records for a specific stream has been
   * reconstructed.
   */
  virtual void consume_step(std::string *id_buffer, std::string *read_buffer,
                            const std::string *quality_buffer, uint32_t count,
                            int stream_index) = 0;
};

/**
 * @brief Standard sink implementation that writes records to FASTQ/FASTA files.
 */
class FileDecompressionSink : public DecompressionSink {
public:
  FileDecompressionSink(const std::string &outfile_1,
                        const std::string &outfile_2,
                        const compression_params &cp, bool use_crlf,
                        const bool (&should_gzip)[2],
                        const bool (&should_bgzf)[2]);
  ~FileDecompressionSink() override;

  void consume_step(std::string *id_buffer, std::string *read_buffer,
                    const std::string *quality_buffer, uint32_t count,
                    int stream_index) override;

private:
  std::ofstream output_streams[2];
  bool should_gzip[2];
  bool should_bgzf[2];
  bool use_crlf;
  bool fasta_mode;
  int compression_level;
  int num_thr;
  bool paired_end;
};

// Short-read archives reconstruct aligned and unaligned records separately.
void decompress_short(const std::string &temp_dir, DecompressionSink &sink,
                      compression_params &cp);

// Long-read archives store read streams directly, without reference-based
// reconstruction.
void decompress_long(const std::string &temp_dir, DecompressionSink &sink,
                     compression_params &cp);

// Packed reference chunks are decoded once, then concatenated by callers.
void decompress_unpack_seq(const std::string &packed_seq_base_path,
                           int encoding_thread_count, int decoding_thread_count,
                           const compression_params &cp);

} // namespace spring

#endif // SPRING_DECOMPRESS_H_
