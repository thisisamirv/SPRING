// Implements preprocessing artifact helpers that encode side streams and
// archive-member payloads apart from generic input normalization.

#include "preprocess_artifacts.h"

#include <array>
#include <cstring>

namespace spring {

void add_archive_member(std::unordered_map<std::string, std::string> &members,
                        const std::string &path,
                        const std::vector<char> &bytes) {
  members[path] = std::string(bytes.begin(), bytes.end());
}

void append_uint16(std::string &buffer, const uint16_t value) {
  buffer.append(reinterpret_cast<const char *>(&value), sizeof(uint16_t));
}

void append_encoded_dna_bits(std::string &buffer, const std::string &read) {
  static const std::array<uint8_t, 128> dna_to_int = []() {
    std::array<uint8_t, 128> table{};
    table[static_cast<uint8_t>('A')] = 0;
    table[static_cast<uint8_t>('C')] = 2;
    table[static_cast<uint8_t>('G')] = 1;
    table[static_cast<uint8_t>('T')] = 3;
    return table;
  }();

  const uint16_t readlen = static_cast<uint16_t>(read.size());
  append_uint16(buffer, readlen);
  const uint32_t encoded_bytes = (static_cast<uint32_t>(readlen) + 3U) / 4U;
  const size_t old_size = buffer.size();
  buffer.resize(old_size + encoded_bytes);
  uint8_t *out = reinterpret_cast<uint8_t *>(&buffer[old_size]);

  for (uint32_t byte_index = 0; byte_index < encoded_bytes; ++byte_index) {
    uint8_t packed = 0;
    for (uint32_t base_index = 0; base_index < 4; ++base_index) {
      const uint32_t read_index = byte_index * 4U + base_index;
      if (read_index >= readlen)
        break;
      const uint8_t encoded =
          dna_to_int[static_cast<uint8_t>(read[read_index])] & 0x03;
      packed |= static_cast<uint8_t>(encoded << (2U * base_index));
    }
    out[byte_index] = packed;
  }
}

void append_encoded_dna_n_bits(std::string &buffer, const std::string &read) {
  static const std::array<uint8_t, 128> dna_n_to_int = []() {
    std::array<uint8_t, 128> table{};
    table[static_cast<uint8_t>('A')] = 0;
    table[static_cast<uint8_t>('C')] = 2;
    table[static_cast<uint8_t>('G')] = 1;
    table[static_cast<uint8_t>('T')] = 3;
    table[static_cast<uint8_t>('N')] = 4;
    return table;
  }();

  const uint16_t readlen = static_cast<uint16_t>(read.size());
  append_uint16(buffer, readlen);
  const uint32_t encoded_bytes = (static_cast<uint32_t>(readlen) + 1U) / 2U;
  const size_t old_size = buffer.size();
  buffer.resize(old_size + encoded_bytes);
  uint8_t *out = reinterpret_cast<uint8_t *>(&buffer[old_size]);

  for (uint32_t byte_index = 0; byte_index < encoded_bytes; ++byte_index) {
    uint8_t packed = 0;
    for (uint32_t base_index = 0; base_index < 2; ++base_index) {
      const uint32_t read_index = byte_index * 2U + base_index;
      if (read_index >= readlen)
        break;
      const uint8_t encoded =
          dna_n_to_int[static_cast<uint8_t>(read[read_index])] & 0x0F;
      packed |= static_cast<uint8_t>(encoded << (4U * base_index));
    }
    out[byte_index] = packed;
  }
}

uint32_t flush_short_read_thread_buffers(
    const std::vector<short_read_thread_buffers> &thread_buffers,
    std::string &clean_output, std::string &n_read_output,
    std::vector<uint32_t> &n_read_order_output, std::string *tail_output,
    std::string *atac_adapter_output) {
  uint32_t clean_read_count = 0;
  for (const short_read_thread_buffers &thread_buffer : thread_buffers) {
    if (!thread_buffer.clean_read_bytes.empty()) {
      clean_output.append(thread_buffer.clean_read_bytes);
    }
    if (!thread_buffer.n_read_bytes.empty()) {
      n_read_output.append(thread_buffer.n_read_bytes);
    }
    if (!thread_buffer.n_read_positions.empty()) {
      n_read_order_output.insert(n_read_order_output.end(),
                                 thread_buffer.n_read_positions.begin(),
                                 thread_buffer.n_read_positions.end());
    }
    if (tail_output && !thread_buffer.tail_info_bytes.empty()) {
      tail_output->append(thread_buffer.tail_info_bytes);
    }
    if (atac_adapter_output && !thread_buffer.atac_adapter_bytes.empty()) {
      atac_adapter_output->append(thread_buffer.atac_adapter_bytes);
    }
    clean_read_count += thread_buffer.clean_read_count;
  }

  return clean_read_count;
}

std::vector<char> build_raw_string_block_bytes(std::string *strings,
                                               const uint32_t string_count,
                                               const uint32_t *string_lengths) {
  size_t total_size = 0;
  for (uint32_t i = 0; i < string_count; i++) {
    total_size += string_lengths[i];
  }

  std::vector<char> output_bytes;
  output_bytes.reserve(total_size);
  for (uint32_t i = 0; i < string_count; i++) {
    output_bytes.insert(output_bytes.end(), strings[i].begin(),
                        strings[i].begin() + string_lengths[i]);
  }
  return output_bytes;
}

std::vector<char> build_read_length_block_bytes(const uint32_t *read_lengths,
                                                const uint32_t read_count) {
  std::vector<char> output_bytes(read_count * sizeof(uint32_t));
  if (read_count > 0) {
    std::memcpy(output_bytes.data(), read_lengths, output_bytes.size());
  }
  return output_bytes;
}

} // namespace spring