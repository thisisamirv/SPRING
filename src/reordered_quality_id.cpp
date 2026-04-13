// Reorders and compresses quality-value and identifier streams so they align
// with Spring's reordered read layout before archive packaging.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <omp.h>
#include <string>
#include <vector>

#include "libbsc/bsc.h"
#include "progress.h"
#include "reordered_quality_id.h"
#include "reordered_streams.h"
#include "util.h"

namespace spring {

namespace {

enum class reorder_compress_mode : uint8_t {
  quality,
  id,
};

struct batch_range {
  uint32_t begin;
  uint32_t end;
};

struct batch_record {
  uint32_t relative_position;
  uint32_t string_length;
};

struct batch_partition_file {
  std::string path;
  std::ofstream output;
  std::string buffer;
};

constexpr size_t kBatchIoBufferSize = 1 << 20;

std::string block_file_path(const std::string &base_path,
                            const uint64_t block_num) {
  return base_path + "." + std::to_string(block_num);
}

uint32_t reads_per_file(const uint32_t num_reads, const bool paired_end) {
  return paired_end ? num_reads / 2 : num_reads;
}

uint32_t compute_batch_size(const uint32_t num_reads,
                            const uint32_t num_reads_per_block) {
  return (1 + (num_reads - 1) / num_reads_per_block) * num_reads_per_block;
}

batch_range batch_read_range(const uint32_t batch_index,
                             const uint32_t batch_size,
                             const uint32_t total_reads) {
  const uint32_t batch_begin = batch_index * batch_size;
  const uint32_t batch_end = std::min(batch_begin + batch_size, total_reads);
  return {.begin = batch_begin, .end = batch_end};
}

uint32_t block_read_count(const uint64_t block_begin,
                          const uint64_t block_end) {
  return static_cast<uint32_t>(block_end - block_begin);
}

uint32_t batch_count_for_reads(const uint32_t total_reads,
                               const uint32_t batch_size) {
  return (total_reads + batch_size - 1) / batch_size;
}

std::string batch_temp_path(const std::string &input_path,
                            const uint32_t batch_index) {
  return input_path + ".batch." + std::to_string(batch_index);
}

void flush_batch_partition_file(batch_partition_file &batch_file) {
  if (batch_file.buffer.empty()) {
    return;
  }

  batch_file.output.write(
      batch_file.buffer.data(),
      static_cast<std::streamsize>(batch_file.buffer.size()));
  batch_file.buffer.clear();
}

void append_batch_record(batch_partition_file &batch_file,
                         const batch_record &record, const std::string &value) {
  const char *record_bytes = byte_ptr(&record);
  batch_file.buffer.append(record_bytes, sizeof(batch_record));
  batch_file.buffer.append(value);

  if (batch_file.buffer.size() >= kBatchIoBufferSize) {
    flush_batch_partition_file(batch_file);
  }
}

void partition_reordered_batches(
    const std::string &input_path,
    const std::vector<uint32_t> &reordered_positions, const uint32_t batch_size,
    std::vector<std::string> &batch_paths) {
  const uint32_t num_batches = batch_count_for_reads(
      static_cast<uint32_t>(reordered_positions.size()), batch_size);
  batch_paths.resize(num_batches);
  std::vector<batch_partition_file> batch_files;
  batch_files.reserve(num_batches);

  for (uint32_t batch_index = 0; batch_index < num_batches; batch_index++) {
    batch_paths[batch_index] = batch_temp_path(input_path, batch_index);
    batch_files.push_back(
        {.path = batch_paths[batch_index],
         .output = std::ofstream(batch_paths[batch_index], std::ios::binary),
         .buffer = {}});
    batch_files.back().buffer.reserve(kBatchIoBufferSize);
  }

  std::ifstream input_stream(input_path);
  std::string current_string;
  for (uint32_t read_index = 0; read_index < reordered_positions.size();
       read_index++) {
    std::getline(input_stream, current_string);
    remove_CR_from_end(current_string);

    const uint32_t reordered_position = reordered_positions[read_index];
    const uint32_t batch_index = reordered_position / batch_size;
    batch_record record{.relative_position = reordered_position % batch_size,
                        .string_length =
                            static_cast<uint32_t>(current_string.size())};
    append_batch_record(batch_files[batch_index], record, current_string);
  }

  for (batch_partition_file &batch_file : batch_files) {
    flush_batch_partition_file(batch_file);
    batch_file.output.close();
  }
}

void load_partitioned_batch(const std::string &batch_path,
                            const uint32_t reads_in_batch,
                            std::vector<std::string> &reordered_strings) {
  std::ifstream batch_input(batch_path, std::ios::binary | std::ios::ate);
  const std::streamsize batch_size = batch_input.tellg();
  batch_input.seekg(0, std::ios::beg);

  std::vector<char> batch_bytes(static_cast<size_t>(batch_size));
  if (batch_size > 0) {
    batch_input.read(batch_bytes.data(), batch_size);
  }

  const char *cursor = batch_bytes.data();
  const char *end = cursor + batch_bytes.size();
  while (cursor < end) {
    batch_record record;
    std::memcpy(&record, cursor, sizeof(batch_record));
    cursor += sizeof(batch_record);
    reordered_strings[record.relative_position].assign(cursor,
                                                       record.string_length);
    cursor += record.string_length;
  }

  for (uint32_t read_index = reads_in_batch;
       read_index < reordered_strings.size(); read_index++) {
    reordered_strings[read_index].clear();
  }
}

void compress_block_batch(const std::string &input_path,
                          const reorder_compress_mode mode,
                          compression_params &cp,
                          std::vector<std::string> &reordered_strings,
                          const batch_range &batch,
                          const uint32_t num_reads_per_block,
                          const int num_threads) {
#pragma omp parallel
  {
    const uint64_t thread_id = omp_get_thread_num();
    const uint64_t block_offset = batch.begin / num_reads_per_block;
    uint64_t block_index = thread_id;
    std::vector<uint32_t> read_lengths;
    if (mode == reorder_compress_mode::quality)
      read_lengths.resize(num_reads_per_block);

    while (true) {
      const uint64_t block_begin = block_index * num_reads_per_block;
      if (block_begin >= batch.end - batch.begin)
        break;

      uint64_t block_end = (block_index + 1) * num_reads_per_block;
      if (block_end > batch.end - batch.begin)
        block_end = batch.end - batch.begin;

      const uint32_t reads_in_block = block_read_count(block_begin, block_end);
      const std::string output_path =
          block_file_path(input_path, block_offset + block_index);

      if (mode == reorder_compress_mode::id) {
        compress_id_block(output_path.c_str(),
                          reordered_strings.data() + block_begin,
                          reads_in_block, cp.encoding.compression_level,
                          true /* pack_only */);
        const uint64_t global_block_idx = block_offset + block_index;
        if (global_block_idx <
            compression_params::ReadMetadata::kFileLenThrSize) {
          cp.read_info.file_len_id_thr[global_block_idx] =
              std::filesystem::file_size(output_path);
        } else {
          throw std::runtime_error(
              std::string("Exceeded maximum supported block count (") +
              std::to_string(
                  compression_params::ReadMetadata::kFileLenThrSize) +
              "). Increase array size in util.h.");
        }
      } else {
        for (uint64_t read_offset = 0; read_offset < reads_in_block;
             read_offset++) {
          read_lengths[read_offset] =
              reordered_strings[block_begin + read_offset].size();
        }
        if (cp.quality.qvz_flag) {
          quantize_quality_qvz(reordered_strings.data() + block_begin,
                               reads_in_block, read_lengths.data(),
                               cp.quality.qvz_ratio);
        }
        bsc::BSC_str_array_compress(output_path.c_str(),
                                    reordered_strings.data() + block_begin,
                                    reads_in_block, read_lengths.data());
      }

      block_index += num_threads;
    }
  }
}

bool should_process_stream(const int stream_index, const bool paired_end,
                           const bool skip_second_stream) {
  if (!paired_end && stream_index == 1)
    return false;
  if (skip_second_stream && stream_index == 1)
    return false;
  return true;
}

// Builds a quality-slot permutation for paired-end mode.
// reordered_positions[raw_R1_pos] = corrected_original_R1_index.
// The sequence stream uses corrected original indices for block assignment
// (see reorder_compress_streams), so quality blocks must use the same
// index space.  Corrected index = raw_pos + number of N-reads inserted before
// raw_pos, which we recover from the n_read_order_path file.
void generate_order_pe(const std::string &base_dir,
                       std::vector<uint32_t> &reordered_positions,
                       const uint32_t num_reads) {
  const uint32_t spot_count = num_reads / 2;
  uint32_t corrected = 0;
  uint32_t orig_spot;

  // 1. Aligned spots
  std::ifstream aligned_input(base_dir + "/read_order.bin", std::ios::binary);
  if (aligned_input.is_open()) {
    while (aligned_input.read(reinterpret_cast<char *>(&orig_spot),
                              sizeof(uint32_t))) {
      if (orig_spot < spot_count)
        reordered_positions[orig_spot] = corrected++;
    }
  }

  // 2. Clean unaligned spots (singletons)
  std::ifstream singleton_input(base_dir + "/read_order.bin.singleton",
                                std::ios::binary);
  if (singleton_input.is_open()) {
    while (singleton_input.read(reinterpret_cast<char *>(&orig_spot),
                                sizeof(uint32_t))) {
      if (orig_spot < spot_count)
        reordered_positions[orig_spot] = corrected++;
    }
  }

  // 3. N-read spots
  std::ifstream n_input(base_dir + "/read_order_N.bin", std::ios::binary);
  if (n_input.is_open()) {
    while (
        n_input.read(reinterpret_cast<char *>(&orig_spot), sizeof(uint32_t))) {
      if (orig_spot < spot_count)
        reordered_positions[orig_spot] = corrected++;
    }
  }
}

void generate_order_se(const std::string &base_dir,
                       std::vector<uint32_t> &reordered_positions,
                       const uint32_t num_reads) {
  uint32_t corrected = 0;
  uint32_t orig_pos;

  // 1. Aligned reads: order defined by the reorderer
  std::ifstream aligned_input(base_dir + "/read_order.bin", std::ios::binary);
  if (aligned_input.is_open()) {
    while (aligned_input.read(reinterpret_cast<char *>(&orig_pos),
                              sizeof(uint32_t))) {
      if (orig_pos < num_reads)
        reordered_positions[orig_pos] = corrected++;
    }
  }

  // 2. Clean unaligned reads (singletons)
  std::ifstream singleton_input(base_dir + "/read_order.bin.singleton",
                                std::ios::binary);
  if (singleton_input.is_open()) {
    while (singleton_input.read(reinterpret_cast<char *>(&orig_pos),
                                sizeof(uint32_t))) {
      if (orig_pos < num_reads)
        reordered_positions[orig_pos] = corrected++;
    }
  }

  // 3. N-reads
  std::ifstream n_input(base_dir + "/read_order_N.bin", std::ios::binary);
  if (n_input.is_open()) {
    while (
        n_input.read(reinterpret_cast<char *>(&orig_pos), sizeof(uint32_t))) {
      if (orig_pos < num_reads)
        reordered_positions[orig_pos] = corrected++;
    }
  }
}

void reorder_compress(const std::string &input_path,
                      const uint32_t num_reads_per_file, const int num_thr,
                      const uint32_t num_reads_per_block,
                      std::vector<std::string> &reordered_strings,
                      const uint32_t batch_size,
                      const std::vector<uint32_t> &reordered_positions,
                      const reorder_compress_mode mode,
                      compression_params &cp) {
  std::vector<std::string> batch_paths;
  partition_reordered_batches(input_path, reordered_positions, batch_size,
                              batch_paths);

  for (uint32_t batch_index = 0;; batch_index++) {
    const batch_range batch =
        batch_read_range(batch_index, batch_size, num_reads_per_file);
    if (batch.begin >= batch.end)
      break;

    load_partitioned_batch(batch_paths[batch_index], batch.end - batch.begin,
                           reordered_strings);
    compress_block_batch(input_path, mode, cp, reordered_strings, batch,
                         num_reads_per_block, num_thr);
    remove(batch_paths[batch_index].c_str());
  }
}

} // namespace

void reorder_compress_quality_id(const std::string &temp_dir,
                                 compression_params &cp) {
  const uint32_t num_reads = cp.read_info.num_reads;
  const int num_thr = cp.encoding.num_thr;
  const bool preserve_id = cp.encoding.preserve_id;
  const bool preserve_quality = cp.encoding.preserve_quality;
  const bool paired_end = cp.encoding.paired_end;
  const uint32_t num_reads_per_block = cp.encoding.num_reads_per_block;
  const bool paired_id_match = cp.read_info.paired_id_match;

  const std::string base_dir = temp_dir;

  std::string id_paths[2];
  std::string quality_paths[2];
  id_paths[0] = base_dir + "/id_1";
  id_paths[1] = base_dir + "/id_2";
  quality_paths[0] = base_dir + "/quality_1";
  quality_paths[1] = base_dir + "/quality_2";

  std::vector<uint32_t> reordered_positions;
  if (paired_end) {
    reordered_positions.resize(num_reads / 2);
    generate_order_pe(base_dir, reordered_positions, num_reads);
  } else {
    reordered_positions.resize(num_reads);
    generate_order_se(base_dir, reordered_positions, num_reads);
  }

  omp_set_num_threads(num_thr);

  const uint32_t batch_size =
      compute_batch_size(num_reads, num_reads_per_block);
  std::vector<std::string> reordered_strings(batch_size);
  // Bound the working set so this stage stays within the reorder memory budget.

  if (preserve_quality) {
    Logger::log_info("Compressing qualities");
    for (int stream_index = 0; stream_index < 2; stream_index++) {
      if (!should_process_stream(stream_index, paired_end, false))
        continue;
      const uint32_t file_read_count = reads_per_file(num_reads, paired_end);
      reorder_compress(quality_paths[stream_index], file_read_count, num_thr,
                       num_reads_per_block, reordered_strings, batch_size,
                       reordered_positions, reorder_compress_mode::quality, cp);
      remove(quality_paths[stream_index].c_str());
    }
  }
  if (preserve_id) {
    Logger::log_info("Compressing ids");
    for (int stream_index = 0; stream_index < 2; stream_index++) {
      if (!should_process_stream(stream_index, paired_end, paired_id_match))
        continue;
      const uint32_t file_read_count = reads_per_file(num_reads, paired_end);
      reorder_compress(id_paths[stream_index], file_read_count, num_thr,
                       num_reads_per_block, reordered_strings, batch_size,
                       reordered_positions, reorder_compress_mode::id, cp);

      // Monolithic ID merge phase: Merge blocks into one file and BSC compress.
      const uint32_t num_blocks =
          (file_read_count + num_reads_per_block - 1) / num_reads_per_block;
      if (num_blocks > compression_params::ReadMetadata::kFileLenThrSize) {
        throw std::runtime_error(
            std::string("Exceeded maximum supported block count (") +
            std::to_string(compression_params::ReadMetadata::kFileLenThrSize) +
            "). Increase array size in util.h.");
      }
      const std::string monolithic_path = id_paths[stream_index] + ".bsc";
      const std::string merged_packed_path = id_paths[stream_index] + ".packed";

      std::ofstream merged_out(merged_packed_path, std::ios::binary);
      if (!merged_out)
        throw std::runtime_error("Failed to open merged packed ID file.");

      for (uint32_t b = 0; b < num_blocks; b++) {
        const std::string block_path =
            block_file_path(id_paths[stream_index], b);
        std::ifstream block_in(block_path, std::ios::binary);
        if (block_in) {
          merged_out << block_in.rdbuf();
          block_in.close();
          remove(block_path.c_str());
        }
      }
      merged_out.close();

      bsc::BSC_compress(merged_packed_path.c_str(), monolithic_path.c_str());
      remove(merged_packed_path.c_str());
      remove(id_paths[stream_index].c_str());
    }
  }
}

} // namespace spring
