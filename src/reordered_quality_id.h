// Declares the quality/id reordering and compression stage that follows read
// reordering during archive construction.

#ifndef SPRING_REORDERED_QUALITY_ID_H_
#define SPRING_REORDERED_QUALITY_ID_H_

#include <string>

namespace spring {

struct compression_params;

// Reorder preserved ids and qualities to match the post-reorder read layout.
void reorder_compress_quality_id(const std::string &temp_dir,
                                 compression_params &cp);

} // namespace spring

#endif // SPRING_REORDERED_QUALITY_ID_H_
