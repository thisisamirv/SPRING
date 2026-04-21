#include "assay_detector.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>
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

std::string get_executable_dir() {
  // A robust way to locate reference would be parameterized, but we assume it's
  // next to the executable or in CWD
  return "reference/ref_hg38_gencode49.fa";
}

} // namespace

void AssayDetector::load_reference(const std::string &ref_path) {
  if (reference_loaded_)
    return;

  std::ifstream in(ref_path);
  if (!in.is_open()) {
    std::cerr << "Warning: Could not open diagnostic reference at " << ref_path
              << ".\n";
    return; // we'll gracefully handle lack of reference in evaluate_stages
  }

  std::string line;
  BlockID current_block = BlockID::NONE;
  std::string current_seq;

  auto process_seq = [&]() {
    if (current_seq.length() >= kKmerSize && current_block != BlockID::NONE) {
      for (size_t i = 0; i <= current_seq.length() - kKmerSize;
           i += 5) { // Subsample k-mers to save memory
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

void AssayDetector::process_reads(const std::string &r1_path,
                                  const std::string &r2_path,
                                  ReadStats &stats) {
  auto read_file = [&](const std::string &path, bool is_r1) {
    if (path.empty())
      return;
    gzFile f = gzopen(path.c_str(), "rb");
    if (!f)
      return;

    char buf[8192];
    int line_idx = 0;
    std::string seq;

    while (gzgets(f, buf, sizeof(buf)) && stats.total_reads < kMaxReadsToScan) {
      std::string line(buf);
      while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
      }

      if (line_idx % 4 == 1) { // Sequence line
        seq = line;
        if (is_r1)
          stats.r1_lengths.push_back(seq.length());
        else
          stats.r2_lengths.push_back(seq.length());

        for (char c : seq) {
          if (c == 'C')
            stats.total_C++;
          else if (c == 'T')
            stats.total_T++;
          else if (c == 'G')
            stats.total_G++;
          else if (c == 'A')
            stats.total_A++;
        }

        if (has_atac_adapter(seq))
          stats.atac_adapters_found++;
        if (has_poly_a_tail(seq))
          stats.poly_a_tails_found++;

        if (reference_loaded_ && seq.length() >= kKmerSize) {
          for (size_t i = 0; i <= seq.length() - kKmerSize;
               i += 10) { // Sketch sampling
            uint64_t code = encode_kmer(seq, i, kKmerSize);
            if (code != std::numeric_limits<uint64_t>::max()) {
              uint64_t can = canonical_kmer(code, kKmerSize);
              stats.total_sampled_kmers++;
              auto it = kmer_index_.find(can);
              if (it != kmer_index_.end()) {
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
                case BlockID::GENOME_BACKBONE:
                  stats.genome_hits++;
                  break;
                default:
                  break;
                }
              }
            }
          }
        }
      } else if (line_idx % 4 == 3) {
        if (is_r1 && r2_path.empty())
          stats.total_reads++; // count per fragment
        else if (!is_r1)
          stats.total_reads++;
      }
      line_idx++;
    }
    gzclose(f);
  };

  read_file(r1_path, true);
  read_file(r2_path, false);
}

std::string AssayDetector::evaluate_stages(const ReadStats &stats,
                                           bool explicit_sc_layout,
                                           std::string &confidence_out) {
  if (stats.total_reads == 0) {
    confidence_out = "low (no reads parsed)";
    return "dna";
  }

  // Stage 1: Methylation
  if (stats.total_reads >= 1000) {
    double c_ratio = static_cast<double>(stats.total_C) /
                     (stats.total_C + stats.total_T + 1);
    double g_ratio = static_cast<double>(stats.total_G) /
                     (stats.total_G + stats.total_A + 1);
    if (c_ratio < 0.05 || g_ratio < 0.05) {
      confidence_out = "high (bisulfite conversion signature)";
      return explicit_sc_layout ? "sc-methyl" : "methyl";
    }
  }

  // Stage 2: Single-cell Layout
  bool is_single_cell = explicit_sc_layout;
  if (!is_single_cell && !stats.r1_lengths.empty() &&
      !stats.r2_lengths.empty()) {
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
    }
  }

  // Stage 3: FASTQ Signatures
  double atac_frac =
      static_cast<double>(stats.atac_adapters_found) / stats.total_reads;
  if (atac_frac > 0.03) {
    confidence_out = "high (Tn5 adapter signature)";
    return is_single_cell ? "sc-atac" : "atac";
  }

  double poly_a_frac =
      static_cast<double>(stats.poly_a_tails_found) / stats.total_reads;
  if (poly_a_frac > 0.03) {
    confidence_out = "high (poly-A/T tail signature)";
    return is_single_cell ? "sc-rna" : "rna";
  }

  // Stage 4: Alignment Sketch
  if (reference_loaded_ && stats.total_sampled_kmers > 0) {
    double total_aligned = stats.rna_hits + stats.atac_hits +
                           stats.intron_hits + stats.genome_hits;
    if (total_aligned < 10) {
      confidence_out = "low (sample may be non-human or contamination)";
      return "dna";
    }

    double rna_score =
        static_cast<double>(stats.rna_hits) / (stats.intron_hits + 1);
    double atac_score =
        static_cast<double>(stats.atac_hits) / (stats.intron_hits + 1);

    if (rna_score > 10.0) {
      confidence_out = "medium (alignment to RNA exons)";
      return is_single_cell ? "sc-rna" : "rna";
    } else if (atac_score > 5.0) {
      confidence_out = "medium (alignment to ATAC promoters)";
      return is_single_cell ? "sc-atac" : "atac";
    } else if (rna_score < 2.0 && atac_score < 2.0) {
      confidence_out = "medium (uniform background alignment)";
      return "dna";
    }
  }

  confidence_out = "low (default)";
  return "dna";
}

std::string
AssayDetector::detect(const std::string &r1_path, const std::string &r2_path,
                      const std::string &r3_path, const std::string &i1_path,
                      const std::string &i2_path, std::string &confidence_out) {
  bool explicit_sc = (!r3_path.empty() || !i1_path.empty() || !i2_path.empty());

  // Attempt to load reference from working directory
  load_reference(get_executable_dir());

  ReadStats stats;
  process_reads(r1_path, r2_path, stats);

  return evaluate_stages(stats, explicit_sc, confidence_out);
}

} // namespace spring
