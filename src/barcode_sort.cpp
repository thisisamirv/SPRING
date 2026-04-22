#include "barcode_sort.h"

#include "io_utils.h"
#include "params.h"
#include "progress.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace spring {

namespace {

// 2-bit decode table matching dna_utils.cpp: A=0,G=1,C=2,T=3
const char kDnaDecodeTable[4] = {'A', 'G', 'C', 'T'};

// Decode the first `prefix_len` bases from a 2-bit packed buffer (same
// encoding as write_dna_in_bits: 4 bases per byte, LSB first).
std::string decode_prefix(const char *packed_buf, uint16_t readlen,
                          uint32_t prefix_len) {
  const uint32_t n = std::min<uint32_t>(prefix_len, readlen);
  std::string result(n, 'A');
  for (uint32_t i = 0; i < n; i++) {
    uint8_t byte = static_cast<uint8_t>(packed_buf[i / 4]);
    result[i] = kDnaDecodeTable[(byte >> (2 * (i % 4))) & 0x03];
  }
  return result;
}

// One packed read as stored in input_clean_1.dna / temp.dna.N:
//   [uint16_t readlen] [ceil(readlen/4) bytes of 2-bit packed DNA]
struct PackedRead {
  std::string cb;       // extracted cellular barcode (opaque bytes)
  uint32_t original_id; // position in input_clean_1.dna (0-based)
  uint16_t readlen;
  std::string packed; // raw payload bytes (NOT the uint16 length)
};

// Load all reads from one preprocessed file (input_clean_1.dna or
// input_clean_2.dna).  If `cb_stream` is non-null, consume one FASTQ record
// from it to obtain the CB for each read; otherwise decode from the packed
// R1 prefix using `cb_len` bases.
// `id_offset` is added to the stored original_id (non-zero for R2 reads).
std::vector<PackedRead> load_reads(const std::string &path, uint32_t id_offset,
                                   uint32_t cb_len, std::istream *cb_stream,
                                   uint32_t &detected_cb_len) {
  std::ifstream fin(path, std::ios::binary);
  if (!fin.is_open())
    throw std::runtime_error("barcode_sort: cannot open " + path);

  std::vector<PackedRead> entries;
  bool first_cb_read = true;

  while (true) {
    uint16_t readlen = 0;
    if (!fin.read(reinterpret_cast<char *>(&readlen), sizeof(uint16_t)))
      break; // EOF

    const uint32_t packed_bytes = (static_cast<uint32_t>(readlen) + 3) / 4;
    std::string packed(packed_bytes, '\0');
    if (!fin.read(packed.data(), packed_bytes))
      throw std::runtime_error("barcode_sort: truncated read in " + path);

    std::string cb;
    if (cb_stream) {
      std::string hdr, seq, plus, qual;
      if (std::getline(*cb_stream, hdr) && std::getline(*cb_stream, seq) &&
          std::getline(*cb_stream, plus) && std::getline(*cb_stream, qual)) {
        // Strip CRLF
        if (!seq.empty() && seq.back() == '\r')
          seq.pop_back();
        if (first_cb_read) {
          detected_cb_len = static_cast<uint32_t>(seq.size());
          first_cb_read = false;
          SPRING_LOG_INFO("barcode_sort: auto-detected cb_len=" +
                          std::to_string(detected_cb_len) + " bp from I1 lane");
        }
        cb = std::move(seq);
      } else {
        // I1 exhausted early — fall back to R1 prefix
        cb = decode_prefix(packed.data(), readlen, cb_len);
      }
    } else {
      cb = decode_prefix(packed.data(), readlen, cb_len);
    }

    entries.push_back(
        {.cb = std::move(cb),
         .original_id = static_cast<uint32_t>(entries.size()) + id_offset,
         .readlen = readlen,
         .packed = std::move(packed)});
  }
  return entries;
}

// Create an empty file (for stub singletons).
void create_empty(const std::string &path) {
  std::ofstream f(path, std::ios::binary);
  if (!f.is_open())
    throw std::runtime_error("barcode_sort: cannot create " + path);
}

// Write the files for one thread slot.  slot_data contains all reads assigned
// to this thread slot in their final sorted order.
void write_thread_slot(const std::string &basedir, int tid,
                       const std::vector<PackedRead *> &slot_data) {
  auto p = [&](const std::string &name) {
    return basedir + "/" + name + "." + std::to_string(tid);
  };

  std::ofstream dna(p("temp.dna"), std::ios::binary);
  std::ofstream rev(p("read_rev.txt"), std::ios::binary);
  std::ofstream flag(p("tempflag.txt"), std::ios::binary);
  std::ofstream pos(p("temppos.txt"), std::ios::binary);
  std::ofstream order(p("read_order.bin"), std::ios::binary);
  std::ofstream lengths(p("read_lengths.bin"), std::ios::binary);

  if (!dna || !rev || !flag || !pos || !order || !lengths)
    throw std::runtime_error(
        "barcode_sort: failed to open output files for thread " +
        std::to_string(tid));

  int64_t position = 0;
  for (const PackedRead *e : slot_data) {
    // temp.dna: [uint16 readlen][packed bytes] — same as write_dna_in_bits
    dna.write(reinterpret_cast<const char *>(&e->readlen), sizeof(uint16_t));
    dna.write(e->packed.data(), static_cast<std::streamsize>(e->packed.size()));
    rev.put('d');
    flag.put('1');
    pos.write(reinterpret_cast<const char *>(&position), sizeof(int64_t));
    order.write(reinterpret_cast<const char *>(&e->original_id),
                sizeof(uint32_t));
    lengths.write(reinterpret_cast<const char *>(&e->readlen),
                  sizeof(uint16_t));
    ++position;
  }
}

} // namespace

void barcode_sort(const std::string &temp_dir, const compression_params &cp,
                  const std::string &cb_source_path) {
  const int num_thr = cp.encoding.num_thr;
  const bool paired_end = cp.encoding.paired_end;
  const uint32_t num_reads_1 = cp.read_info.num_reads_clean[0];
  const uint32_t num_reads_2 = paired_end ? cp.read_info.num_reads_clean[1] : 0;

  SPRING_LOG_INFO("Barcode sort: " +
                  std::string(cb_source_path.empty()
                                  ? "CB from R1 prefix (" +
                                        std::to_string(cp.encoding.cb_len) +
                                        " bp)"
                                  : "CB from I1 lane (auto-detecting length)"));

  // ── Open I1 stream if available ─────────────────────────────────────────
  std::unique_ptr<gzip_istream> i1_gz;
  std::ifstream i1_plain;
  std::istream *i1_stream = nullptr;

  if (!cb_source_path.empty()) {
    const bool is_gz = cb_source_path.ends_with(".gz");
    if (is_gz) {
      i1_gz = std::make_unique<gzip_istream>(cb_source_path);
      i1_stream = i1_gz.get();
    } else {
      i1_plain.open(cb_source_path, std::ios::binary);
      if (!i1_plain.is_open())
        throw std::runtime_error("barcode_sort: cannot open I1 source: " +
                                 cb_source_path);
      i1_stream = &i1_plain;
    }
  }

  uint32_t detected_cb_len = cp.encoding.cb_len;

  // ── Load R1 reads ────────────────────────────────────────────────────────
  SPRING_LOG_INFO("barcode_sort: loading reads...");
  std::vector<PackedRead> reads_1 =
      load_reads(temp_dir + "/input_clean_1.dna", 0, cp.encoding.cb_len,
                 i1_stream, detected_cb_len);

  if (reads_1.size() != num_reads_1) {
    SPRING_LOG_DEBUG("barcode_sort: expected " + std::to_string(num_reads_1) +
                     " R1 reads, loaded " + std::to_string(reads_1.size()));
  }

  // ── Load R2 reads (paired-end) ───────────────────────────────────────────
  std::vector<PackedRead> reads_2;
  if (paired_end) {
    uint32_t dummy_cb_len = detected_cb_len;
    // R2 CB is not extracted; we use an empty string so R2 follows R1 in sort
    reads_2 = load_reads(temp_dir + "/input_clean_2.dna", num_reads_1,
                         cp.encoding.cb_len, nullptr, dummy_cb_len);
    if (reads_2.size() != num_reads_2) {
      SPRING_LOG_DEBUG("barcode_sort: expected " + std::to_string(num_reads_2) +
                       " R2 reads, loaded " + std::to_string(reads_2.size()));
    }
  }

  // ── Build the sort index for R1 (R2 mirrors R1 via index) ───────────────
  // For paired-end: sort R1 by CB, R2 follows in lock-step.
  const uint32_t total_reads =
      static_cast<uint32_t>(reads_1.size() + reads_2.size());
  SPRING_LOG_INFO("barcode_sort: stable-sorting " +
                  std::to_string(reads_1.size()) + " read pairs by CB...");

  // Build index into reads_1; sort by CB string only (stable).
  std::vector<uint32_t> r1_order(reads_1.size());
  for (uint32_t i = 0; i < static_cast<uint32_t>(r1_order.size()); ++i)
    r1_order[i] = i;
  std::ranges::stable_sort(r1_order, [&](uint32_t a, uint32_t b) {
    return reads_1[a].cb < reads_1[b].cb;
  });

  // ── Distribute reads across thread slots (round-robin) ──────────────────
  // Each thread slot is a contiguous file pair read by the encoder.
  // We keep CB-group locality by assigning whole groups to the same slot.
  std::vector<std::vector<PackedRead *>> slots_1(num_thr);
  std::vector<std::vector<PackedRead *>> slots_2(num_thr);

  for (uint32_t slot = 0; slot < static_cast<uint32_t>(r1_order.size());
       ++slot) {
    const int tid = static_cast<int>(slot % static_cast<uint32_t>(num_thr));
    slots_1[static_cast<size_t>(tid)].push_back(&reads_1[r1_order[slot]]);
    if (paired_end)
      slots_2[static_cast<size_t>(tid)].push_back(&reads_2[r1_order[slot]]);
  }

  // ── Write per-thread cascade files ──────────────────────────────────────
  // For the encoder, R1 and R2 thread files are separate streams.
  // In the existing pipeline R1 clean reads occupy [0, num_reads_1) and
  // R2 occupy [num_reads_1, num_reads_1+num_reads_2) in a single stream.
  // writetofile merges R1 then R2 per thread, so we do the same.
  SPRING_LOG_INFO("barcode_sort: writing sorted output files...");
  for (int tid = 0; tid < num_thr; ++tid) {
    // Merge R1 and R2 slot data into one sequence (R1 first, then R2).
    std::vector<PackedRead *> merged = slots_1[static_cast<size_t>(tid)];
    if (paired_end) {
      const auto &s2 = slots_2[static_cast<size_t>(tid)];
      merged.insert(merged.end(), s2.begin(), s2.end());
    }
    write_thread_slot(temp_dir, tid, merged);

    // Empty singleton stub for this thread slot
    create_empty(temp_dir + "/temp.dna.singleton." + std::to_string(tid));
    create_empty(temp_dir + "/read_order.bin.singleton." + std::to_string(tid));
  }

  // ── Merged singleton stubs (consumed by readsingletons()) ───────────────
  create_empty(temp_dir + "/temp.dna.singleton");
  create_empty(temp_dir + "/read_order.bin.singleton");

  {
    std::ofstream cnt(temp_dir + "/temp.dna.singleton.count", std::ios::binary);
    if (!cnt.is_open())
      throw std::runtime_error(
          "barcode_sort: cannot write singleton count file");
    const uint32_t zero = 0;
    cnt.write(reinterpret_cast<const char *>(&zero), sizeof(uint32_t));
  }

  SPRING_LOG_INFO("barcode_sort: done — " + std::to_string(total_reads) +
                  " reads sorted by CB.");
}

} // namespace spring
