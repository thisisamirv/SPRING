// Declares the stream-reordering stage that materializes archive-ready position,
// noise, and unaligned data after reads have been reordered.

#ifndef SPRING_REORDERED_STREAMS_H_
#define SPRING_REORDERED_STREAMS_H_

#include <string>

namespace spring {

struct compression_params;

// Rebuild the aligned and unaligned side streams into per-block archives.
void reorder_compress_streams(const std::string &temp_dir,
                              const compression_params &cp);

} // namespace spring

#endif // SPRING_REORDERED_STREAMS_H_
