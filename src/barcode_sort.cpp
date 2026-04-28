#include "barcode_sort.h"
#include "dna_utils.h"
#include "fs_utils.h"
#include "io_utils.h"
#include "params.h"
#include "progress.h"

#include <algorithm>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
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
  std::string raw_read; // raw DNA bases (ASCII)
  bool is_n = false;    // true when loaded from input_N.dna (4-bit packed)
};

// Load sharded DNA reads from Clean and N streams, reconstructing their
// original order.
std::vector<PackedRead> load_all_reads(const std::string &temp_dir,
                                       int stream_index, uint32_t total_reads,
                                       uint32_t num_clean, uint32_t num_n,
                                       uint32_t &detected_cb_len,
                                       std::istream *cb_stream,
                                       uint32_t cb_len) {
  std::vector<PackedRead> entries(total_reads);
  std::vector<bool> is_n(total_reads, false);

  const std::string n_order_path =
      temp_dir + "/read_order_N.bin" + (stream_index == 1 ? ".2" : "");
  const std::string n_dna_path =
      temp_dir + "/input_N.dna" + (stream_index == 1 ? ".2" : "");
  const std::string clean_dna_path =
      temp_dir + "/input_clean_" + std::to_string(stream_index + 1) + ".dna";

  // 1. Identify which positions are N-reads using the order file.
  if (num_n > 0) {
    std::ifstream fn_order(n_order_path, std::ios::binary);
    if (fn_order.is_open()) {
      for (uint32_t i = 0; i < num_n; i++) {
        uint32_t pos;
        if (fn_order.read(reinterpret_cast<char *>(&pos), sizeof(uint32_t))) {
          if (pos < total_reads)
            is_n[pos] = true;
        }
      }
    }
  }

  // 2. Load N-reads (4-bit packed) and place them at their original positions.
  if (num_n > 0) {
    std::ifstream fn_dna(n_dna_path, std::ios::binary);
    if (fn_dna.is_open()) {
      static const char kDnaNDecodeTable[16] = {'A', 'G', 'C', 'T', 'N', 'N',
                                                'N', 'N', 'N', 'N', 'N', 'N',
                                                'N', 'N', 'N', 'N'};
      for (uint32_t i = 0; i < total_reads; i++) {
        if (!is_n[i])
          continue;
        uint16_t readlen;
        if (!fn_dna.read(reinterpret_cast<char *>(&readlen), sizeof(uint16_t)))
          break;
        uint32_t packed_bytes = (static_cast<uint32_t>(readlen) + 1) / 2;
        std::vector<char> packed(packed_bytes);
        fn_dna.read(packed.data(), packed_bytes);

        entries[i].readlen = readlen;
        entries[i].original_id = i;
        entries[i].is_n = true;
        entries[i].raw_read.resize(readlen);
        for (uint32_t j = 0; j < readlen; j++) {
          uint8_t byte = static_cast<uint8_t>(packed[j / 2]);
          uint8_t code = (j % 2 == 0) ? (byte & 0x0F) : (byte >> 4);
          entries[i].raw_read[j] = kDnaNDecodeTable[code & 0x0F];
        }
      }
    }
  }

  // 3. Load Clean reads (2-bit packed) and fill the remaining slots.
  if (num_clean > 0) {
    std::ifstream fc_dna(clean_dna_path, std::ios::binary);
    if (fc_dna.is_open()) {
      for (uint32_t i = 0; i < total_reads; i++) {
        if (is_n[i])
          continue;
        uint16_t readlen;
        if (!fc_dna.read(reinterpret_cast<char *>(&readlen), sizeof(uint16_t)))
          break;
        uint32_t packed_bytes = (static_cast<uint32_t>(readlen) + 3) / 4;
        std::vector<char> packed(packed_bytes);
        fc_dna.read(packed.data(), packed_bytes);

        entries[i].readlen = readlen;
        entries[i].original_id = i;
        entries[i].raw_read.resize(readlen);
        for (uint32_t j = 0; j < readlen; j++) {
          uint8_t byte = static_cast<uint8_t>(packed[j / 4]);
          entries[i].raw_read[j] =
              kDnaDecodeTable[(byte >> (2 * (j % 4))) & 0x03];
        }
      }
    }
  }

  // 4. Extract or decode CBs for all reads (only for R1).
  if (stream_index == 0) {
    bool first_cb_read = true;
    for (uint32_t i = 0; i < total_reads; i++) {
      if (cb_stream) {
        std::string hdr, seq, plus, qual;
        if (std::getline(*cb_stream, hdr) && std::getline(*cb_stream, seq) &&
            std::getline(*cb_stream, plus) && std::getline(*cb_stream, qual)) {
          if (!seq.empty() && seq.back() == '\r')
            seq.pop_back();
          if (first_cb_read) {
            detected_cb_len = static_cast<uint32_t>(seq.size());
            first_cb_read = false;
            SPRING_LOG_INFO("barcode_sort: auto-detected cb_len=" +
                            std::to_string(detected_cb_len) +
                            " bp from I1 lane");
          }
          entries[i].cb = std::move(seq);
        } else {
          // I1 exhausted — fallback to prefix if possible
          entries[i].cb =
              (cb_len > 0)
                  ? entries[i].raw_read.substr(
                        0, std::min<uint32_t>(cb_len, entries[i].readlen))
                  : "";
        }
      } else if (cb_len > 0) {
        entries[i].cb = entries[i].raw_read.substr(
            0, std::min<uint32_t>(cb_len, entries[i].readlen));
      }
    }
  }

  return entries;
}

// Returns true when a barcode string looks like an undetermined-well sequence:
// more than 80% of its bases are the same character (including N).
bool is_undetermined_barcode(const std::string &cb) {
  if (cb.size() < 4)
    return false;
  std::array<uint32_t, 5> cnt = {}; // A G C T other
  for (char c : cb) {
    switch (c) {
    case 'A':
      cnt[0]++;
      break;
    case 'G':
      cnt[1]++;
      break;
    case 'C':
      cnt[2]++;
      break;
    case 'T':
      cnt[3]++;
      break;
    default:
      cnt[4]++;
      break;
    }
  }
  const uint32_t thresh = static_cast<uint32_t>(cb.size()) * 4 / 5;
  for (uint32_t c : cnt)
    if (c > thresh)
      return true;
  return false;
}

// Reorder a CB group into greedy overlap-chain order using k-mer prefix
// matching. 'seq_reads' is the read vector used for overlap detection —
// for sc-rna paired-end this is reads_2 (cDNA), otherwise reads_1.
// min_overlap matches THRESH_ENCODER so we pre-sort exactly the overlaps the
// encoder will subsequently exploit, improving chain starts and length.
std::vector<uint32_t>
overlap_chain_sort(const std::vector<uint32_t> &group,
                   const std::vector<PackedRead> &seq_reads,
                   uint32_t min_overlap) {
  const uint32_t n = static_cast<uint32_t>(group.size());
  if (n <= 1)
    return group;

  // Map: first min_overlap chars of each read -> local indices within group.
  std::unordered_map<std::string, std::vector<uint32_t>> prefix_map;
  prefix_map.reserve(n);
  for (uint32_t i = 0; i < n; i++) {
    const PackedRead &r = seq_reads[group[i]];
    if (r.readlen >= static_cast<uint16_t>(min_overlap) && !r.is_n)
      prefix_map[r.raw_read.substr(0, min_overlap)].push_back(i);
  }

  std::vector<bool> used(n, false);
  std::vector<uint32_t> result;
  result.reserve(n);

  for (uint32_t i = 0; i < n; i++) {
    if (used[i])
      continue;
    used[i] = true;
    result.push_back(group[i]);
    uint32_t cur = i;
    while (true) {
      const PackedRead &r = seq_reads[group[cur]];
      if (r.readlen < static_cast<uint16_t>(min_overlap))
        break;
      // Suffix of current read — look for a read whose prefix matches it.
      const std::string suffix = r.raw_read.substr(r.readlen - min_overlap);
      auto it = prefix_map.find(suffix);
      if (it == prefix_map.end())
        break;
      uint32_t nxt = UINT32_MAX;
      for (uint32_t j : it->second) {
        if (!used[j]) {
          nxt = j;
          break;
        }
      }
      if (nxt == UINT32_MAX)
        break;
      used[nxt] = true;
      result.push_back(group[nxt]);
      cur = nxt;
    }
  }
  return result;
}

// Write cb_scan_order.bin so that call_reorder picks seeds in CB-sorted order.
// This is written when barcode_sort falls back to call_reorder due to low CB
// diversity: it gives call_reorder a head start by grouping same-cell reads
// together as consecutive seed candidates, improving overlap chain length.
void write_cb_scan_order(const std::string &temp_dir,
                         const std::vector<PackedRead> &reads_1,
                         const std::vector<PackedRead> &reads_2,
                         const std::vector<uint32_t> &r1_order,
                         uint32_t num_clean_1, bool paired_end) {
  // Build global-position → clean-stream-position map for R1.
  std::vector<uint32_t> r1_clean_pos(reads_1.size(), UINT32_MAX);
  uint32_t c1 = 0;
  for (uint32_t i = 0; i < static_cast<uint32_t>(reads_1.size()); i++) {
    if (!reads_1[i].is_n)
      r1_clean_pos[i] = c1++;
  }

  std::vector<uint32_t> order;
  order.reserve(c1 + (paired_end ? c1 : 0));

  for (uint32_t idx : r1_order) {
    if (!reads_1[idx].is_n)
      order.push_back(r1_clean_pos[idx]);
  }

  if (paired_end) {
    std::vector<uint32_t> r2_clean_pos(reads_2.size(), UINT32_MAX);
    uint32_t c2 = 0;
    for (uint32_t i = 0; i < static_cast<uint32_t>(reads_2.size()); i++) {
      if (!reads_2[i].is_n)
        r2_clean_pos[i] = c2++;
    }
    for (uint32_t idx : r1_order) {
      if (!reads_2[idx].is_n)
        order.push_back(num_clean_1 + r2_clean_pos[idx]);
    }
  }

  if (order.empty())
    return;

  std::ofstream f(temp_dir + "/cb_scan_order.bin", std::ios::binary);
  if (!f.is_open())
    throw std::runtime_error("barcode_sort: cannot write cb_scan_order.bin");
  f.write(reinterpret_cast<const char *>(order.data()),
          static_cast<std::streamsize>(order.size() * sizeof(uint32_t)));

  SPRING_LOG_INFO("barcode_sort: wrote CB scan order for " +
                  std::to_string(order.size()) +
                  " clean reads -> call_reorder");
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
    write_dna_in_bits(e->raw_read, dna);
    rev.put('d');
    flag.put('1');
    pos.write(reinterpret_cast<const char *>(&position), sizeof(int64_t));
    order.write(reinterpret_cast<const char *>(&e->original_id),
                sizeof(uint32_t));
    lengths.write(reinterpret_cast<const char *>(&e->readlen),
                  sizeof(uint16_t));
    position += e->readlen;
  }
}

} // namespace

bool barcode_sort(const std::string &temp_dir, compression_params &cp,
                  const std::string &cb_source_path) {
  const int num_thr = cp.encoding.num_thr;
  const bool paired_end = cp.encoding.paired_end;
  // Total reads per stream is half total_reads in PE, or total_reads in SE.
  const uint32_t reads_per_stream =
      paired_end ? cp.read_info.num_reads / 2 : cp.read_info.num_reads;

  // For both sc-ATAC and sc-RNA, call_reorder's global overlap search
  // outperforms CB-grouped encoding:
  //
  //   sc-ATAC: reads within a cell come from different genomic loci, so there
  //     are no within-cell sequence overlaps. Grouping by CB only prevents the
  //     encoder from chaining reads from the same genomic peak across cells.
  //
  //   sc-RNA: cross-cell chains (same transcript expressed in many cells) are
  //     the dominant compression source. Isolating cells into separate encoder
  //     slots severs those chains. A single large CB group also produces an
  //     unbalanced, oversized encoder dictionary that hurts chain quality.
  //
  // The CB-sort hint (cb_scan_order.bin) is also skipped: for ATAC it biases
  // seed selection away from genomic-overlap clusters; for RNA the gain is
  // negligible (~0%) compared with the overhead of loading all reads just to
  // write the hint file.
  if (cp.read_info.assay == "sc-atac" || cp.read_info.assay == "sc-rna") {
    SPRING_LOG_INFO("barcode_sort disabled for " + cp.read_info.assay +
                    ": using overlap-based reordering.");
    return false;
  }

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

  // ── Load R1 reads (clean + N) ────────────────────────────────────────────
  SPRING_LOG_INFO("barcode_sort: loading reads...");
  std::vector<PackedRead> reads_1 = load_all_reads(
      temp_dir, 0, reads_per_stream, cp.read_info.num_reads_clean[0],
      reads_per_stream - cp.read_info.num_reads_clean[0], detected_cb_len,
      i1_stream, cp.encoding.cb_len);

  // ── Load R2 reads (paired-end) ───────────────────────────────────────────
  std::vector<PackedRead> reads_2;
  if (paired_end) {
    uint32_t r2_detected_len = detected_cb_len;
    const uint32_t r2_cb_len = detected_cb_len;
    reads_2 = load_all_reads(temp_dir, 1, reads_per_stream,
                             cp.read_info.num_reads_clean[1],
                             reads_per_stream - cp.read_info.num_reads_clean[1],
                             r2_detected_len, nullptr, r2_cb_len);

    // Paired-end read_order.bin uses global read indices where R2 occupies the
    // second half [reads_per_stream, 2*reads_per_stream). This keeps
    // downstream quality/id reorder mapping consistent.
    for (PackedRead &entry : reads_2) {
      entry.original_id += reads_per_stream;
    }
  }

  // ── Build the sort index for R1 (R2 mirrors R1 via index) ───────────────
  const uint32_t total_reads_loaded =
      static_cast<uint32_t>(reads_1.size() + reads_2.size());
  SPRING_LOG_INFO("barcode_sort: stable-sorting " +
                  std::to_string(reads_1.size()) + " read pairs by CB...");

  std::vector<uint32_t> r1_order(reads_1.size());
  for (uint32_t i = 0; i < static_cast<uint32_t>(r1_order.size()); ++i)
    r1_order[i] = i;
  std::ranges::stable_sort(r1_order, [&](uint32_t a, uint32_t b) {
    return reads_1[a].cb < reads_1[b].cb;
  });

  uint64_t cb_unique = 0;
  uint64_t current_run = 0;
  uint64_t dominant_run = 0;
  std::string previous_cb;
  std::string dominant_cb;
  for (uint32_t order_index : r1_order) {
    const std::string &cb = reads_1[order_index].cb;
    if (current_run == 0 || cb != previous_cb) {
      if (current_run > dominant_run) {
        dominant_run = current_run;
        dominant_cb = previous_cb;
      }
      cb_unique++;
      current_run = 1;
      previous_cb = cb;
    } else {
      current_run++;
    }
  }
  if (current_run > dominant_run) {
    dominant_run = current_run;
    dominant_cb = previous_cb;
  }
  const uint64_t read_count = static_cast<uint64_t>(r1_order.size());
  // Truly degenerate: fewer than 10 unique barcodes, or the dominant barcode
  // is an undetermined-well sequence (>80% of one character, including N).
  // A biologically skewed but valid distribution (e.g. one large clone) is no
  // longer penalised — barcode grouping still helps within that clone.
  const bool low_barcode_diversity =
      (read_count >= 10000) &&
      (cb_unique < 10 || is_undetermined_barcode(dominant_cb));
  if (low_barcode_diversity) {
    Logger::log_warning(
        "barcode_sort disabled: degenerate CB distribution (reads=" +
        std::to_string(read_count) + ", unique_cb=" +
        std::to_string(cb_unique) + ", dominant_cb=" + dominant_cb +
        "). Falling back to overlap reordering with CB presort hint.");
    write_cb_scan_order(temp_dir, reads_1, reads_2, r1_order,
                        cp.read_info.num_reads_clean[0], paired_end);
    return false;
  }

  // ── Two-stage slot fill ─────────────────────────────────────────────────
  //
  // Stage 1 (within-CB call_reorder): for each CB group, greedily chain reads
  //   by k-mer overlap so the encoder's seed selection hits chain starts first,
  //   producing longer and tighter overlap chains.  For paired-end data we
  //   sort by R2 (cDNA / genomic), which carries the actual transcript overlap
  //   structure; R1 (CB+UMI) has no useful within-group overlap pattern.
  //
  // Stage 2 (slot assignment): whole CB groups are assigned to the
  //   least-loaded thread slot (greedy min-load) so within-cell chains remain
  //   contiguous inside one slot and the encoder can extend them freely.
  std::vector<std::vector<PackedRead *>> slots_1(num_thr);
  std::vector<std::vector<PackedRead *>> slots_2(num_thr);
  {
    std::vector<uint32_t> slot_load(static_cast<size_t>(num_thr), 0);
    uint32_t pos = 0;
    const uint32_t total = static_cast<uint32_t>(r1_order.size());

    while (pos < total) {
      // Identify the end of the current CB group.
      uint32_t end = pos + 1;
      while (end < total &&
             reads_1[r1_order[end]].cb == reads_1[r1_order[pos]].cb)
        ++end;

      // Stage 1: order this CB group by greedy overlap chains.
      // Use R2 for paired-end (cDNA carries the transcript overlap signal);
      // fall back to R1 for single-end.
      std::vector<uint32_t> group(r1_order.begin() + pos,
                                  r1_order.begin() + end);
      {
        const std::vector<PackedRead> &seq =
            (paired_end && !reads_2.empty()) ? reads_2 : reads_1;
        group = overlap_chain_sort(group, seq, THRESH_ENCODER);
      }

      // Stage 2: assign the whole CB group to the least-loaded slot.
      auto min_it = std::min_element(slot_load.begin(), slot_load.end());
      const int tid = static_cast<int>(min_it - slot_load.begin());

      for (uint32_t idx : group) {
        slots_1[static_cast<size_t>(tid)].push_back(&reads_1[idx]);
        if (paired_end)
          slots_2[static_cast<size_t>(tid)].push_back(&reads_2[idx]);
      }
      *min_it += end - pos;

      pos = end;
    }
  }

  // ── Write per-thread cascade files ──────────────────────────────────────
  SPRING_LOG_INFO("barcode_sort: writing sorted output files...");
  for (int tid = 0; tid < num_thr; ++tid) {
    std::vector<PackedRead *> merged = slots_1[static_cast<size_t>(tid)];
    if (paired_end) {
      const auto &s2 = slots_2[static_cast<size_t>(tid)];
      merged.insert(merged.end(), s2.begin(), s2.end());
    }
    write_thread_slot(temp_dir, tid, merged);

    create_empty(temp_dir + "/temp.dna.singleton." + std::to_string(tid));
    create_empty(temp_dir + "/read_order.bin.singleton." + std::to_string(tid));
  }

  create_empty(temp_dir + "/temp.dna.singleton");
  create_empty(temp_dir + "/read_order.bin.singleton");
  create_empty(temp_dir + "/input_N.dna");
  create_empty(temp_dir + "/read_order_N.bin");
  if (paired_end) {
    create_empty(temp_dir + "/input_N.dna.2");
    create_empty(temp_dir + "/read_order_N.bin.2");
  }

  // These preprocess inputs are consumed by barcode_sort and should not be
  // archived as payload; remove them to avoid inflated bundle sizes.
  safe_remove_file(temp_dir + "/input_clean_1.dna");
  if (paired_end) {
    safe_remove_file(temp_dir + "/input_clean_2.dna");
  }

  {
    std::ofstream cnt(temp_dir + "/temp.dna.singleton.count", std::ios::binary);
    if (!cnt.is_open())
      throw std::runtime_error(
          "barcode_sort: cannot write singleton count file");
    const uint32_t zero = 0;
    cnt.write(reinterpret_cast<const char *>(&zero), sizeof(uint32_t));
  }

  // After barcode sorting, all reads are represented in aligned stream files.
  cp.read_info.num_reads_clean[0] = reads_per_stream;
  cp.read_info.num_reads_clean[1] = paired_end ? reads_per_stream : 0;

  SPRING_LOG_INFO("barcode_sort: done — " + std::to_string(total_reads_loaded) +
                  " reads sorted by CB.");
  return true;
}

} // namespace spring
