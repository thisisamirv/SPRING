// Declares the quality/id reordering and compression stage that follows read
// reordering during archive construction.

#ifndef SPRING_REORDERED_QUALITY_ID_H_
#define SPRING_REORDERED_QUALITY_ID_H_

#include <array>
#include <string>
#include <vector>

namespace spring {

struct compression_params;

struct post_encode_side_stream_artifact {
  std::array<std::string, 2> raw_id_streams;
  std::array<std::string, 2> raw_quality_streams;
  std::array<std::string, 2> raw_tail_streams;
  std::array<std::string, 2> compressed_atac_adapter_streams;
};

post_encode_side_stream_artifact
capture_post_encode_side_streams(const std::string &temp_dir,
                                 const compression_params &cp);

// Reorder preserved ids and qualities to match the post-reorder read layout.
void reorder_compress_quality_id(
    const std::string &temp_dir,
    const post_encode_side_stream_artifact &artifact,
    const std::vector<uint32_t> &read_order_entries, compression_params &cp);

} // namespace spring

#endif // SPRING_REORDERED_QUALITY_ID_H_
