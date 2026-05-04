// Declares the helper used to rewrite paired-end ordering after the main read
// reordering stage updates mate positions.

#ifndef SPRING_PAIRED_END_ORDER_H_
#define SPRING_PAIRED_END_ORDER_H_

#include <cstdint>
#include <vector>

namespace spring {

struct compression_params;

// Rewrite paired-end read order so mate pairs land in decompression order.
void pe_encode(std::vector<uint32_t> &read_order_entries,
               const compression_params &cp);

} // namespace spring

#endif // SPRING_PAIRED_END_ORDER_H_
