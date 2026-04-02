// Declares the helper used to rewrite paired-end ordering after the main read
// reordering stage updates mate positions.

#ifndef SPRING_PAIRED_END_ORDER_H_
#define SPRING_PAIRED_END_ORDER_H_

#include <string>

namespace spring {

struct compression_params;

// Rewrite paired-end read order so mate pairs land in decompression order.
void pe_encode(const std::string &temp_dir, const compression_params &cp);

} // namespace spring

#endif // SPRING_PAIRED_END_ORDER_H_
