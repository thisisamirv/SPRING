// Implements single-cell RNA-specific helpers used for barcode-prefix handling
// and grouped index identifier reconstruction.

#include "assay_sc_rna.h"

#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace spring {

SingleCellDetectionEvidence
detect_sc_rna_layout(const AssayDetectionStats &stats,
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

bool is_grouped_index_archive_note(const std::string &note) {
  return note.find("index-group") != std::string::npos;
}

bool is_grouped_read3_archive_note(const std::string &note) {
  return note.find("read3-group") != std::string::npos;
}

bool should_enable_sc_rna_cb_prefix_stripping(const compression_params &cp) {
  return !cp.encoding.long_flag && cp.encoding.preserve_order &&
         !cp.encoding.cb_prefix_source_external && cp.encoding.cb_len > 0 &&
         cp.read_info.assay == "sc-rna" &&
         !is_grouped_index_archive_note(cp.read_info.note);
}

bool should_enable_grouped_sc_rna_index_suffix_stripping(
    const compression_params &cp) {
  return !cp.encoding.long_flag && cp.encoding.preserve_order &&
         cp.encoding.preserve_id && cp.read_info.assay == "sc-rna" &&
         is_grouped_index_archive_note(cp.read_info.note);
}

bool strip_grouped_sc_rna_index_suffix_from_id(std::string &id,
                                               const std::string &stream_1_seq,
                                               bool paired_index) {
  const size_t last_colon = id.rfind(':');
  if (last_colon == std::string::npos || last_colon + 1 >= id.size()) {
    return false;
  }

  std::string_view suffix{id};
  suffix.remove_prefix(last_colon + 1);
  if (paired_index) {
    const size_t plus = suffix.find('+');
    if (plus == std::string::npos || plus == 0) {
      return false;
    }
    if (suffix.substr(0, plus) != stream_1_seq) {
      return false;
    }
  } else if (suffix != stream_1_seq) {
    return false;
  }

  id.resize(last_colon + 1);
  return true;
}

void append_grouped_sc_rna_index_suffix_to_id(std::string &id,
                                              const std::string &index_read_1,
                                              const std::string *index_read_2) {
  if (id.empty() || id.back() != ':') {
    throw std::runtime_error("Corrupt grouped sc-RNA index metadata: stripped "
                             "ID prefix missing ':'");
  }

  id.append(index_read_1);
  if (index_read_2) {
    id.push_back('+');
    id.append(*index_read_2);
  }
}

} // namespace spring
