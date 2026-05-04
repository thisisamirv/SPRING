// Implements sc-ATAC-specific preprocessing and reconstruction helpers so the
// single-cell ATAC path can be tracked independently from bulk ATAC.

#include "assay_sc_atac.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>

namespace spring {

namespace {
constexpr std::array<std::string_view, 2> kScAtacAdapters = {
    "CTGTCTCTTATACACATCT", "AGATGTGTATAAGAGACAG"};
}

SingleCellDetectionEvidence
detect_sc_atac_layout(const AssayDetectionStats &stats,
                      bool explicit_sc_layout) {
  SingleCellDetectionEvidence result;
  result.is_single_cell = explicit_sc_layout;

  if (explicit_sc_layout) {
    result.evidence.push_back("explicit lanes");
    result.indicator_count++;
  }

  if (stats.total_reads == 0) {
    return result;
  }

  const double cb_tag_frac =
      static_cast<double>(stats.headers_with_cb_tag) / stats.total_reads;
  if (cb_tag_frac > 0.5) {
    result.is_single_cell = true;
    result.indicator_count++;
    result.evidence.push_back("CB tags");
  }

  const double umi_tag_frac =
      static_cast<double>(stats.headers_with_umi_tag) / stats.total_reads;
  if (umi_tag_frac > 0.5) {
    result.is_single_cell = true;
    result.indicator_count++;
    result.evidence.push_back("UMI tags");
  }

  if (!stats.r1_lengths.empty() && !stats.r2_lengths.empty()) {
    std::vector<int> r1_copy = stats.r1_lengths;
    std::vector<int> r2_copy = stats.r2_lengths;
    std::nth_element(r1_copy.begin(), r1_copy.begin() + r1_copy.size() / 2,
                     r1_copy.end());
    std::nth_element(r2_copy.begin(), r2_copy.begin() + r2_copy.size() / 2,
                     r2_copy.end());
    const int med_r1 = r1_copy[r1_copy.size() / 2];
    const int med_r2 = r2_copy[r2_copy.size() / 2];

    if (med_r1 <= 45 && (med_r2 - med_r1) >= 30) {
      result.is_single_cell = true;
      result.indicator_count++;
      result.evidence.push_back("read length asymmetry");
    }
  }

  return result;
}

bool should_consider_sc_atac_adapter_stripping(const compression_params &cp,
                                               bool fasta_input) {
  return !fasta_input && !cp.encoding.long_flag &&
         cp.read_info.assay == "sc-atac" &&
         (cp.read_info.assay_confidence == "N/A" ||
          cp.read_info.assay_confidence.starts_with("high") ||
          cp.read_info.assay_confidence.starts_with("medium"));
}

bool should_activate_sc_atac_adapter_stripping(uint32_t eligible_reads,
                                               uint64_t stripped_bases,
                                               double min_bases_per_read) {
  const double avg_stripped_bases_per_read =
      eligible_reads == 0 ? 0.0
                          : static_cast<double>(stripped_bases) /
                                static_cast<double>(eligible_reads);
  return avg_stripped_bases_per_read >= min_bases_per_read;
}

uint8_t detect_sc_atac_adapter_tail_info(const std::string &read_str,
                                         uint32_t read_length) {
  if (read_length < ATAC_ADAPTER_MIN_MATCH)
    return 0;

  for (uint16_t adapter_id = 0; adapter_id < kScAtacAdapters.size();
       ++adapter_id) {
    const std::string_view adapter = kScAtacAdapters[adapter_id];
    const uint32_t max_overlap =
        std::min<uint32_t>(read_length, static_cast<uint32_t>(adapter.size()));
    for (uint32_t overlap = max_overlap; overlap >= ATAC_ADAPTER_MIN_MATCH;
         --overlap) {
      const size_t read_offset = static_cast<size_t>(read_length - overlap);
      if (read_str.compare(read_offset, overlap,
                           adapter.substr(0, overlap).data(), overlap) == 0) {
        return static_cast<uint8_t>((overlap << 1) | adapter_id);
      }
      if (overlap == ATAC_ADAPTER_MIN_MATCH)
        break;
    }
  }

  return 0;
}

void strip_sc_atac_adapter_tail(std::string &read_str, uint32_t &read_length,
                                uint8_t adapter_info) {
  const uint32_t strip_len = adapter_info >> 1;
  if (strip_len == 0)
    return;
  read_length -= strip_len;
  read_str.resize(read_length);
}

void restore_sc_atac_adapter_tail(std::string &read_str,
                                  std::string *quality_str,
                                  uint8_t adapter_info,
                                  const std::string *tail_qual) {
  if (adapter_info == 0)
    return;

  const uint8_t adapter_id = adapter_info & 1;
  const uint32_t overlap = adapter_info >> 1;
  const std::string_view adapter = kScAtacAdapters[adapter_id];
  if (overlap > adapter.size()) {
    throw std::runtime_error("Corrupt sc-ATAC adapter metadata: overlap "
                             "exceeds adapter length");
  }

  read_str.append(adapter.substr(0, overlap));
  if (quality_str) {
    if (tail_qual) {
      quality_str->append(*tail_qual);
    } else {
      quality_str->append(overlap, 'I');
    }
  }
}

} // namespace spring
