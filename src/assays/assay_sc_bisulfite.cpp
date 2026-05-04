// Implements sc-bisulfite-specific policy helpers so single-cell bisulfite
// handling is tracked in its own assay file.

#include "assay_sc_bisulfite.h"

#include <algorithm>

namespace spring {

SingleCellDetectionEvidence
detect_sc_bisulfite_layout(const AssayDetectionStats &stats,
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

bool is_sc_bisulfite_assay(const compression_params &cp) {
  return cp.read_info.assay == "sc-bisulfite";
}

bool should_enable_sc_bisulfite_cb_prefix_stripping(
    const compression_params &cp) {
  (void)cp;
  return false;
}

void apply_sc_bisulfite_auto_config(
    compression_params &cp,
    const AssayDetector::DetectionResult &detection_result) {
  if (detection_result.assay != "sc-bisulfite") {
    return;
  }
  if (detection_result.c_ratio < 0.05 || detection_result.g_ratio < 0.05) {
    cp.encoding.bisulfite_ternary = true;
    cp.encoding.depleted_base = detection_result.depleted_base;
  }
}

} // namespace spring
