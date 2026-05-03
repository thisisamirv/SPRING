// Declares the runtime-to-template dispatch helpers that pick reorder and
// encoder implementations sized for the current dataset.

#ifndef SPRING_TEMPLATE_DISPATCH_H_
#define SPRING_TEMPLATE_DISPATCH_H_

#include <string>

#include "reordered_streams.h"

namespace spring {

struct compression_params;

// Bridge runtime read lengths to the explicitly instantiated template entry
// points used by reorder and encoder.
void call_reorder(const std::string &temp_dir, compression_params &cp);

reordered_stream_artifact call_encoder(const std::string &temp_dir,
                                       compression_params &cp);

} // namespace spring

#endif // SPRING_TEMPLATE_DISPATCH_H_
