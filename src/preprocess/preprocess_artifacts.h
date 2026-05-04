// Declares internal preprocessing artifact helpers used to encode side streams
// and archive members separately from generic record normalization.

#ifndef SPRING_PREPROCESS_ARTIFACTS_H_
#define SPRING_PREPROCESS_ARTIFACTS_H_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace spring {

struct short_read_thread_buffers {
  std::string clean_read_bytes;
  std::string n_read_bytes;
  std::vector<uint32_t> n_read_positions;
  std::string tail_info_bytes;
  std::string atac_adapter_bytes;
  uint32_t clean_read_count = 0;
};

void add_archive_member(std::unordered_map<std::string, std::string> &members,
                        const std::string &path,
                        const std::vector<char> &bytes);

void append_uint16(std::string &buffer, uint16_t value);

void append_encoded_dna_bits(std::string &buffer, const std::string &read);

void append_encoded_dna_n_bits(std::string &buffer, const std::string &read);

uint32_t flush_short_read_thread_buffers(
    const std::vector<short_read_thread_buffers> &thread_buffers,
    std::string &clean_output, std::string &n_read_output,
    std::vector<uint32_t> &n_read_order_output, std::string *tail_output,
    std::string *atac_adapter_output);

std::vector<char> build_raw_string_block_bytes(std::string *strings,
                                               uint32_t string_count,
                                               const uint32_t *string_lengths);

std::vector<char> build_read_length_block_bytes(const uint32_t *read_lengths,
                                                uint32_t read_count);

} // namespace spring

#endif // SPRING_PREPROCESS_ARTIFACTS_H_