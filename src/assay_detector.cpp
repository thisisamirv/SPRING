#include "assay_detector.h"
#include "io_utils.h"
#include <algorithm>
#include <limits>
#include <sstream>
#include <string>
#include <vector>
#include <zlib.h>

namespace spring {

namespace {

// Encode 17-mer to a 34-bit integer
uint64_t encode_kmer(const std::string &seq, size_t start, int k) {
  uint64_t code = 0;
  for (int i = 0; i < k; ++i) {
    char c = seq[start + i];
    uint64_t val = 0;
    switch (c) {
    case 'A':
    case 'a':
      val = 0;
      break;
    case 'C':
    case 'c':
      val = 1;
      break;
    case 'G':
    case 'g':
      val = 2;
      break;
    case 'T':
    case 't':
      val = 3;
      break;
    default:
      return static_cast<uint64_t>(-1); // N or invalid
    }
    code = (code << 2) | val;
  }
  return code;
}

uint64_t reverse_complement_code(uint64_t code, int k) {
  uint64_t rc = 0;
  for (int i = 0; i < k; ++i) {
    uint64_t val = (code >> (i * 2)) & 3;
    rc = (rc << 2) | (3 - val);
  }
  return rc;
}

uint64_t canonical_kmer(uint64_t code, int k) {
  uint64_t rc = reverse_complement_code(code, k);
  return code < rc ? code : rc;
}

bool has_non_acgtn_symbol_for_sampling(const std::string &read) {
  return std::ranges::any_of(read, [](char base) {
    if (base == '\r' || base == '\n' || base == ' ')
      return false;
    return base != 'A' && base != 'C' && base != 'G' && base != 'T' &&
           base != 'N';
  });
}

void remove_sample_cr(std::string &line, bool &stream_saw_crlf) {
  if (!line.empty() && line.back() == '\r') {
    stream_saw_crlf = true;
    line.pop_back();
  }
}

} // namespace

// Declared in generated/reference_data.cpp
extern const unsigned char kEmbeddedReference[];
extern const std::size_t kEmbeddedReferenceCompressedSize;
extern const std::size_t kEmbeddedReferenceUncompressedSize;

void AssayDetector::load_reference() {
  if (reference_loaded_)
    return;

  std::string raw(kEmbeddedReferenceUncompressedSize, '\0');
  uLongf dest_len = static_cast<uLongf>(kEmbeddedReferenceUncompressedSize);
  if (uncompress(reinterpret_cast<Bytef *>(raw.data()), &dest_len,
                 kEmbeddedReference,
                 static_cast<uLong>(kEmbeddedReferenceCompressedSize)) != Z_OK)
    return;
  raw.resize(dest_len);

  std::istringstream in(std::move(raw));
  std::string line;
  BlockID current_block = BlockID::NONE;
  std::string current_seq;

  auto process_seq = [&]() {
    if (current_seq.length() >= kKmerSize && current_block != BlockID::NONE) {
      for (size_t i = 0; i <= current_seq.length() - kKmerSize; i += 5) {
        uint64_t code = encode_kmer(current_seq, i, kKmerSize);
        if (code != std::numeric_limits<uint64_t>::max()) {
          uint64_t can = canonical_kmer(code, kKmerSize);
          kmer_index_[can] = current_block;
        }
      }
    }
  };

  while (std::getline(in, line)) {
    if (line.empty())
      continue;
    if (line[0] == '>') {
      process_seq();
      current_seq.clear();
      if (line.find("RNA_EXON") != std::string::npos)
        current_block = BlockID::RNA_EXON;
      else if (line.find("ATAC_PROMOTER") != std::string::npos)
        current_block = BlockID::ATAC_PROMOTER;
      else if (line.find("DNA_INTRON_CTRL") != std::string::npos)
        current_block = BlockID::INTRON_CTRL;
      else if (line.find("GENOME_BACKBONE") != std::string::npos)
        current_block = BlockID::GENOME_BACKBONE;
      else
        current_block = BlockID::NONE;
    } else {
      current_seq += line;
    }
  }
  process_seq();
  reference_loaded_ = true;
}

bool AssayDetector::has_atac_adapter(const std::string &seq) {
  const std::string kmer1 = "CTGTCTCTTATA";
  const std::string kmer2 = "TATACACATCTC";
  return seq.find(kmer1) != std::string::npos ||
         seq.find(kmer2) != std::string::npos;
}

bool AssayDetector::has_poly_a_tail(const std::string &seq) {
  if (seq.length() < 20)
    return false;
  int a_count = 0, t_count = 0;
  for (size_t i = seq.length() - 20; i < seq.length(); ++i) {
    if (seq[i] == 'A')
      a_count++;
    if (seq[i] == 'T')
      t_count++;
  }
  return (a_count >= 15 || t_count >= 15);
}

AssayDetector::StartupAnalysisResult AssayDetector::analyze_startup_sample(
    const std::string &r1_path, const std::string &r2_path,
    const std::string &r3_path, const std::string &i1_path,
    const std::string &i2_path, const bool paired_end, const bool fasta_input) {
  const bool explicit_sc =
      (!r3_path.empty() || !i1_path.empty() || !i2_path.empty());

  load_reference();

  StartupAnalysisResult result;
  ReadStats stats;

  auto record_sequence = [&](const std::string &header, const std::string &seq,
                             const bool is_r1, const int stream_index) {
    result.input_summary.max_read_length =
        std::max(result.input_summary.max_read_length,
                 static_cast<uint32_t>(seq.size()));
    if (!result.input_summary.contains_non_acgtn_symbols &&
        has_non_acgtn_symbol_for_sampling(seq)) {
      result.input_summary.contains_non_acgtn_symbols = true;
    }

    if (header.find("CB:Z:") != std::string::npos) {
      stats.headers_with_cb_tag++;
    }
    if (header.find("UB:Z:") != std::string::npos ||
        header.find("UR:Z:") != std::string::npos ||
        header.find("UMI:") != std::string::npos) {
      stats.headers_with_umi_tag++;
    }

    if (is_r1) {
      stats.r1_lengths.push_back(static_cast<int>(seq.length()));
    } else {
      stats.r2_lengths.push_back(static_cast<int>(seq.length()));
    }

    for (const char c : seq) {
      if (is_r1) {
        if (c == 'C')
          stats.r1_C++;
        else if (c == 'T')
          stats.r1_T++;
        else if (c == 'G')
          stats.r1_G++;
        else if (c == 'A')
          stats.r1_A++;
      } else {
        if (c == 'C')
          stats.r2_C++;
        else if (c == 'T')
          stats.r2_T++;
        else if (c == 'G')
          stats.r2_G++;
        else if (c == 'A')
          stats.r2_A++;
      }
    }

    if (has_atac_adapter(seq)) {
      stats.atac_adapters_found++;
    }
    if (has_poly_a_tail(seq)) {
      stats.poly_a_tails_found++;
    }

    if (reference_loaded_ && seq.length() >= kKmerSize) {
      for (size_t i = 0; i <= seq.length() - kKmerSize; i += 10) {
        const uint64_t code = encode_kmer(seq, i, kKmerSize);
        if (code == std::numeric_limits<uint64_t>::max()) {
          continue;
        }
        const uint64_t can = canonical_kmer(code, kKmerSize);
        stats.total_sampled_kmers++;
        const auto it = kmer_index_.find(can);
        if (it == kmer_index_.end()) {
          continue;
        }
        switch (it->second) {
        case BlockID::RNA_EXON:
          stats.rna_hits++;
          break;
        case BlockID::ATAC_PROMOTER:
          stats.atac_hits++;
          break;
        case BlockID::INTRON_CTRL:
          stats.intron_hits++;
          break;
        default:
          break;
        }
      }
    }

    (void)stream_index;
  };

  if (!fasta_input) {
    gzip_istream r1_stream(r1_path);
    if (!r1_stream.is_open()) {
      result.assay_result.assay = "dna";
      result.assay_result.confidence = "low (no reads parsed)";
      return result;
    }
    gzip_istream r2_stream;
    if (paired_end) {
      r2_stream.open(r2_path);
      if (!r2_stream.is_open()) {
        result.assay_result.assay = "dna";
        result.assay_result.confidence = "low (no reads parsed)";
        return result;
      }
    }

    auto read_fastq_record = [](std::istream &stream, std::string &header,
                                std::string &seq, bool &stream_saw_crlf) {
      std::string plus_line;
      std::string quality_line;
      if (!std::getline(stream, header)) {
        return false;
      }
      if (!std::getline(stream, seq) || !std::getline(stream, plus_line) ||
          !std::getline(stream, quality_line)) {
        throw std::runtime_error("Invalid FASTQ while sampling startup reads.");
      }
      remove_sample_cr(header, stream_saw_crlf);
      remove_sample_cr(seq, stream_saw_crlf);
      remove_sample_cr(plus_line, stream_saw_crlf);
      remove_sample_cr(quality_line, stream_saw_crlf);
      return true;
    };

    for (int sampled = 0; sampled < kMaxReadsToScan; ++sampled) {
      std::string header_1;
      std::string seq_1;
      if (!read_fastq_record(r1_stream, header_1, seq_1,
                             result.input_summary.use_crlf_by_stream[0])) {
        break;
      }
      record_sequence(header_1, seq_1, true, 0);

      if (paired_end) {
        std::string header_2;
        std::string seq_2;
        if (!read_fastq_record(r2_stream, header_2, seq_2,
                               result.input_summary.use_crlf_by_stream[1])) {
          throw std::runtime_error(
              "Paired-end input truncated during startup sampling.");
        }
        record_sequence(header_2, seq_2, false, 1);
      }

      stats.total_reads++;
      result.input_summary.sampled_fragments++;
    }
  } else {
    gzip_istream r1_stream(r1_path);
    if (!r1_stream.is_open()) {
      result.assay_result.assay = "dna";
      result.assay_result.confidence = "low (no reads parsed)";
      return result;
    }
    gzip_istream r2_stream;
    if (paired_end) {
      r2_stream.open(r2_path);
      if (!r2_stream.is_open()) {
        result.assay_result.assay = "dna";
        result.assay_result.confidence = "low (no reads parsed)";
        return result;
      }
    }

    auto read_fasta_record =
        [](std::istream &stream, std::string &pending_header,
           std::string &header, std::string &seq, bool &stream_saw_crlf) {
          header.clear();
          seq.clear();
          std::string line;
          if (!pending_header.empty()) {
            header = pending_header;
            pending_header.clear();
          } else {
            while (std::getline(stream, line)) {
              remove_sample_cr(line, stream_saw_crlf);
              if (!line.empty() && line.front() == '>') {
                header = line;
                break;
              }
            }
          }
          if (header.empty()) {
            return false;
          }
          while (std::getline(stream, line)) {
            remove_sample_cr(line, stream_saw_crlf);
            if (!line.empty() && line.front() == '>') {
              pending_header = line;
              break;
            }
            seq += line;
          }
          return true;
        };

    std::string pending_header_1;
    std::string pending_header_2;
    for (int sampled = 0; sampled < kMaxReadsToScan; ++sampled) {
      std::string header_1;
      std::string seq_1;
      if (!read_fasta_record(r1_stream, pending_header_1, header_1, seq_1,
                             result.input_summary.use_crlf_by_stream[0])) {
        break;
      }
      record_sequence(header_1, seq_1, true, 0);

      if (paired_end) {
        std::string header_2;
        std::string seq_2;
        if (!read_fasta_record(r2_stream, pending_header_2, header_2, seq_2,
                               result.input_summary.use_crlf_by_stream[1])) {
          throw std::runtime_error(
              "Paired-end FASTA input truncated during startup sampling.");
        }
        record_sequence(header_2, seq_2, false, 1);
      }

      stats.total_reads++;
      result.input_summary.sampled_fragments++;
    }
  }

  result.assay_result = evaluate_stages(stats, explicit_sc);
  return result;
}

AssayDetector::DetectionResult
AssayDetector::evaluate_stages(const ReadStats &stats,
                               bool explicit_sc_layout) {
  DetectionResult res;
  if (stats.total_reads == 0) {
    res.assay = "dna";
    res.confidence = "low (no reads parsed)";
    return res;
  }

  // === PHASE 1: Compute evidence scores for each assay type ===

  // Base composition ratios
  double r1_c_ratio =
      static_cast<double>(stats.r1_C) / (stats.r1_C + stats.r1_T + 1);
  double r2_c_ratio =
      static_cast<double>(stats.r2_C) / (stats.r2_C + stats.r2_T + 1);
  double r1_g_ratio =
      static_cast<double>(stats.r1_G) / (stats.r1_G + stats.r1_A + 1);
  double r2_g_ratio =
      static_cast<double>(stats.r2_G) / (stats.r2_G + stats.r2_A + 1);

  // Initialize scores (0-100 scale, higher = more confident)
  double bisulfite_score = 0.0;
  double atac_score = 0.0;
  double rna_score = 0.0;
  double dna_score = 10.0; // baseline for generic DNA

  std::vector<std::string> evidence; // Track what contributed to decision

  // --- Bisulfite Evidence ---
  if (stats.total_reads >= 500) {
    bool r1_c_strong = (stats.r1_C + stats.r1_T > 100) && (r1_c_ratio <= 0.10);
    bool r1_g_strong = (stats.r1_G + stats.r1_A > 100) && (r1_g_ratio <= 0.10);
    bool r2_c_strong = (stats.r2_C + stats.r2_T > 100) && (r2_c_ratio <= 0.10);
    bool r2_g_strong = (stats.r2_G + stats.r2_A > 100) && (r2_g_ratio <= 0.10);

    bool r1_c_moderate =
        (stats.r1_C + stats.r1_T > 100) && (r1_c_ratio <= 0.15);
    bool r1_g_moderate =
        (stats.r1_G + stats.r1_A > 100) && (r1_g_ratio <= 0.15);
    bool r2_c_moderate =
        (stats.r2_C + stats.r2_T > 100) && (r2_c_ratio <= 0.15);
    bool r2_g_moderate =
        (stats.r2_G + stats.r2_A > 100) && (r2_g_ratio <= 0.15);

    // Strong depletion = very high confidence
    if (r1_c_strong || r1_g_strong) {
      bisulfite_score += 80.0;
      evidence.push_back("R1 " + std::string(r1_c_strong ? "C" : "G") +
                         "-depletion");
      res.depleted_base = r1_c_strong ? 'C' : 'G';
    }
    if (r2_c_strong || r2_g_strong) {
      bisulfite_score += 80.0;
      evidence.push_back("R2 " + std::string(r2_c_strong ? "C" : "G") +
                         "-depletion");
      if (res.depleted_base == 'N')
        res.depleted_base = r2_c_strong ? 'C' : 'G';
    }

    // Moderate depletion = medium confidence
    if (!r1_c_strong && !r1_g_strong && (r1_c_moderate || r1_g_moderate)) {
      bisulfite_score += 40.0;
      evidence.push_back("R1 moderate " +
                         std::string(r1_c_moderate ? "C" : "G") + "-depletion");
    }
    if (!r2_c_strong && !r2_g_strong && (r2_c_moderate || r2_g_moderate)) {
      bisulfite_score += 40.0;
      evidence.push_back("R2 moderate " +
                         std::string(r2_c_moderate ? "C" : "G") + "-depletion");
    }

    res.c_ratio = std::min(r1_c_ratio, r2_c_ratio);
    res.g_ratio = std::min(r1_g_ratio, r2_g_ratio);
  }

  // --- ATAC Evidence ---
  double atac_adapter_frac =
      static_cast<double>(stats.atac_adapters_found) / stats.total_reads;
  if (atac_adapter_frac > 0.05) {
    atac_score += 90.0;
    evidence.push_back("Tn5 adapters (>5%)");
  } else if (atac_adapter_frac > 0.01) {
    atac_score += 70.0;
    evidence.push_back("Tn5 adapters");
  } else if (atac_adapter_frac > 0.003) {
    atac_score += 35.0;
    evidence.push_back("Tn5 adapters (weak)");
  }

  // Reference alignment to ATAC promoters
  if (reference_loaded_ && stats.total_sampled_kmers > 0) {
    double atac_ref_score =
        static_cast<double>(stats.atac_hits) / (stats.intron_hits + 1);
    if (atac_ref_score > 5.0) {
      atac_score += 50.0;
      evidence.push_back("promoter alignment");
    } else if (atac_ref_score > 2.0) {
      atac_score += 25.0;
    }
  }

  // --- RNA Evidence ---
  double poly_a_frac =
      static_cast<double>(stats.poly_a_tails_found) / stats.total_reads;

  // Suppress poly-A/T if bisulfite signal present (prevents false positives)
  bool has_bisulfite_signal = (bisulfite_score > 30.0);

  if (poly_a_frac > 0.10 && !has_bisulfite_signal) {
    rna_score += 80.0;
    evidence.push_back("poly-A/T tails (>10%)");
  } else if (poly_a_frac > 0.03 && !has_bisulfite_signal) {
    rna_score += 60.0;
    evidence.push_back("poly-A/T tails");
  }

  // Reference alignment to RNA exons
  if (reference_loaded_ && stats.total_sampled_kmers > 0) {
    double rna_ref_score =
        static_cast<double>(stats.rna_hits) / (stats.intron_hits + 1);
    if (rna_ref_score > 10.0) {
      rna_score += 60.0;
      evidence.push_back("exon alignment");
    } else if (rna_ref_score > 5.0) {
      rna_score += 30.0;
    }
  }

  // === PHASE 2: Determine single-cell layout ===

  bool is_single_cell = explicit_sc_layout;
  int sc_indicator_count = 0;
  std::vector<std::string> sc_evidence;

  if (explicit_sc_layout) {
    sc_evidence.push_back("explicit lanes");
    sc_indicator_count++;
  }

  // CB/UMI tags in headers
  double cb_tag_frac =
      static_cast<double>(stats.headers_with_cb_tag) / stats.total_reads;
  if (cb_tag_frac > 0.5) {
    is_single_cell = true;
    sc_indicator_count++;
    sc_evidence.push_back("CB tags");
  }

  double umi_tag_frac =
      static_cast<double>(stats.headers_with_umi_tag) / stats.total_reads;
  if (umi_tag_frac > 0.5) {
    sc_indicator_count++;
    sc_evidence.push_back("UMI tags");
  }

  // Read length asymmetry (classic 10x pattern: short R1, long R2)
  if (!stats.r1_lengths.empty() && !stats.r2_lengths.empty()) {
    std::vector<int> r1_copy = stats.r1_lengths;
    std::vector<int> r2_copy = stats.r2_lengths;
    std::nth_element(r1_copy.begin(), r1_copy.begin() + r1_copy.size() / 2,
                     r1_copy.end());
    std::nth_element(r2_copy.begin(), r2_copy.begin() + r2_copy.size() / 2,
                     r2_copy.end());
    int med_r1 = r1_copy[r1_copy.size() / 2];
    int med_r2 = r2_copy[r2_copy.size() / 2];

    if (med_r1 <= 45 && (med_r2 - med_r1) >= 30) {
      is_single_cell = true;
      sc_indicator_count++;
      sc_evidence.push_back("read length asymmetry");
    }
  }

  // === PHASE 3: Make final decision ===

  std::string base_assay;
  std::string confidence_level;

  // Select assay with highest score
  double max_score =
      std::max({bisulfite_score, atac_score, rna_score, dna_score});

  if (max_score < 20.0) {
    base_assay = "dna";
    confidence_level = "low";
    evidence.clear();
    evidence.push_back("default");
  } else if (bisulfite_score == max_score && bisulfite_score >= 20.0) {
    base_assay = "bisulfite";
    confidence_level = (bisulfite_score >= 70.0) ? "high" : "medium";
  } else if (atac_score == max_score && atac_score >= 20.0) {
    base_assay = "atac";
    confidence_level = (atac_score >= 70.0) ? "high" : "medium";
  } else if (rna_score == max_score && rna_score >= 20.0) {
    base_assay = "rna";
    confidence_level = (rna_score >= 60.0) ? "high" : "medium";
  } else {
    base_assay = "dna";
    confidence_level = "low";
    evidence.push_back("ambiguous signals");
  }

  // Apply single-cell modifier
  res.assay = is_single_cell ? ("sc-" + base_assay) : base_assay;

  // Build confidence string
  std::string confidence_detail;
  for (size_t i = 0; i < evidence.size(); ++i) {
    if (i > 0)
      confidence_detail += ", ";
    confidence_detail += evidence[i];
  }

  if (is_single_cell && sc_indicator_count > 0) {
    confidence_detail += "; sc: ";
    for (size_t i = 0; i < sc_evidence.size(); ++i) {
      if (i > 0)
        confidence_detail += ", ";
      confidence_detail += sc_evidence[i];
    }
  }

  res.confidence = confidence_level + " (" + confidence_detail + ")";

  return res;
}

AssayDetector::DetectionResult
AssayDetector::detect(const std::string &r1_path, const std::string &r2_path,
                      const std::string &r3_path, const std::string &i1_path,
                      const std::string &i2_path) {
  return analyze_startup_sample(r1_path, r2_path, r3_path, i1_path, i2_path,
                                !r2_path.empty(), false)
      .assay_result;
}

} // namespace spring
