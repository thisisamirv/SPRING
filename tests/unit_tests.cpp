#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "assay_detector.h"
#include "dna_utils.h"
#include "doctest.h"
#include "parse_utils.h"
#include <filesystem>
#include <fstream>
#include <string>

// Suppress a known false-positive from the clang static analyzer where
// doctest's temporary/result macros can confuse the analyzer about
// heap allocations inside `doctest::String`.
// NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)

using namespace spring;

namespace {
TEST_CASE("Testing reverse_complement") {
  SUBCASE("Standard DNA sequences") {
    CHECK(reverse_complement("ATGC", 4) == "GCAT");
    CHECK(reverse_complement("GCTA", 4) == "TAGC");
    CHECK(reverse_complement("AAAA", 4) == "TTTT");
    CHECK(reverse_complement("CCCC", 4) == "GGGG");
  }

  SUBCASE("Sequences with N") { CHECK(reverse_complement("ANN", 3) == "NNT"); }

  SUBCASE("Empty string") { CHECK(reverse_complement("", 0) == ""); }
}

TEST_CASE("Testing suffix check") {
  CHECK(has_suffix("filename.fastq.gz", ".gz") == true);
  CHECK(has_suffix("filename.fastq.gz", ".fastq.gz") == true);
  CHECK(has_suffix("filename.fastq.gz", ".fastq") == false);
  CHECK(has_suffix("filename.fastq.gz", "") == true);
  CHECK(has_suffix("", ".gz") == false);
}

TEST_CASE("Testing integer parsing") {
  CHECK(parse_int_or_throw("123", "err") == 123);
  CHECK(parse_int_or_throw("-456", "err") == -456);
  CHECK_THROWS_AS(parse_int_or_throw("abc", "err"), std::runtime_error);
  CHECK_THROWS_AS(parse_int_or_throw("", "err"), std::runtime_error);
}

TEST_CASE("Testing double parsing") {
  CHECK(parse_double_or_throw("1.23", "err") == doctest::Approx(1.23));
  CHECK(parse_double_or_throw("-4.56", "err") == doctest::Approx(-4.56));
  CHECK_THROWS_AS(parse_double_or_throw("abc", "err"), std::runtime_error);
}

TEST_CASE("Testing uint64 parsing") {
  CHECK(parse_uint64_or_throw("123456789012345", "err") == 123456789012345ULL);
  CHECK_THROWS_AS(parse_uint64_or_throw("-1", "err"), std::runtime_error);
}

TEST_CASE("Testing AssayDetector") {
  AssayDetector detector;

  SUBCASE("No reads parsed") {
    AssayDetector::DetectionResult res = detector.detect("", "", "", "", "");
    std::string assay = res.assay;
    std::string confidence = res.confidence;
    CHECK(assay == "dna");
    CHECK(confidence == "low (no reads parsed)");
  }

  SUBCASE(
      "Test poly-A detection signature directly (if accessible or via files)") {
    // Since process_reads reads files, we create a small temporary fastq file
    std::string test_fq = "test_sc_rna.fq";
    std::ofstream out(test_fq);
    // Write 100 reads with poly-A tails
    for (int i = 0; i < 100; ++i) {
      out << "@read" << i << "\n";
      out << "GCTAGCTAGCTAGCTAGCTAGCTAGCTAGCAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n";
      out << "+\n";
      out << "IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII\n";
    }
    out.close();

    AssayDetector::DetectionResult res =
        detector.detect(test_fq, "", "", "", "");
    std::string assay = res.assay;
    std::string confidence = res.confidence;
    CHECK(assay == "rna");
    CHECK(confidence == "high (poly-A/T tail signature)");

    std::filesystem::remove(test_fq);
  }

  SUBCASE("Test ATAC Tn5 detection signature directly") {
    std::string test_fq = "test_sc_atac.fq";
    std::ofstream out(test_fq);
    // Write 100 reads with Tn5 adapter CTGTCTCTTATA
    for (int i = 0; i < 100; ++i) {
      out << "@read" << i << "\n";
      out << "GCTAGCTAGCTCTGTCTCTTATAGCTAGCTAGCTAGCTAGCTAGCTAGCTAGCTAGCTAG\n";
      out << "+\n";
      out << "IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII\n";
    }
    out.close();

    AssayDetector::DetectionResult res =
        detector.detect(test_fq, "", "", "i1.fq", "");
    std::string assay = res.assay;
    std::string confidence = res.confidence;
    CHECK(assay == "sc-atac");
    CHECK(confidence == "high (Tn5 adapter signature)");

    std::filesystem::remove(test_fq);
  }

  SUBCASE("Test Methylation signature directly") {
    std::string test_fq = "test_methyl.fq";
    std::ofstream out(test_fq);
    // Write 1001 reads with mostly T and no C (bisulfite conversion)
    for (int i = 0; i < 1001; ++i) {
      out << "@read" << i << "\n";
      out << "GTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTTGTTT\n";
      out << "+\n";
      out << "IIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIIII\n";
    }
    out.close();

    AssayDetector::DetectionResult res =
        detector.detect(test_fq, "", "", "", "");
    std::string assay = res.assay;
    std::string confidence = res.confidence;
    CHECK(assay == "methyl");
    CHECK(confidence.find("high (bisulfite conversion signature") !=
          std::string::npos);
    CHECK(res.depleted_base == 'C');

    std::filesystem::remove(test_fq);
  }
}

TEST_CASE("Testing AssayDetector with Real Data") {
  AssayDetector detector;

  // Try to find the data and reference directories
  std::string base_path = ".";
  if (!std::filesystem::exists("assets/sample-data") ||
      !std::filesystem::exists("reference")) {
    base_path = "..";
  }

  std::string data_dir = base_path + "/assets/sample-data/";
  std::string ref_path = base_path + "/reference/ref_hg38_gencode49.fa";

  if (!std::filesystem::exists(ref_path)) {
    // Skip if reference is missing (e.g. in some build environments)
    return;
  }

  SUBCASE("Detect Methylation (test_3)") {
    std::string r1 = data_dir + "test_3_R1.fastq.gz";
    std::string r2 = data_dir + "test_3_R2.fastq.gz";
    if (std::filesystem::exists(r1) && std::filesystem::exists(r2)) {
      AssayDetector::DetectionResult res = detector.detect(r1, r2, "", "", "");
      CHECK(res.assay == "methyl");
      CHECK(res.confidence.find("high (bisulfite conversion signature") !=
            std::string::npos);
    }
  }

  SUBCASE("Detect sc-ATAC (test_4)") {
    std::string r1 = data_dir + "test_4_R1.fastq.gz";
    std::string r2 = data_dir + "test_4_R2.fastq.gz";
    std::string r3 = data_dir + "test_4_R3.fastq.gz";
    std::string i1 = data_dir + "test_4_I1.fastq.gz";
    if (std::filesystem::exists(r1)) {
      AssayDetector::DetectionResult res = detector.detect(r1, r2, r3, i1, "");
      CHECK(res.assay == "sc-atac");
      CHECK(res.confidence == "high (Tn5 adapter signature)");
    }
  }

  SUBCASE("Detect sc-RNA (test_5)") {
    std::string r1 = data_dir + "test_5_R1.fastq.gz";
    std::string r2 = data_dir + "test_5_R2.fastq.gz";
    std::string i1 = data_dir + "test_5_I1.fastq.gz";
    std::string i2 = data_dir + "test_5_I2.fastq.gz";
    if (std::filesystem::exists(r1)) {
      AssayDetector::DetectionResult res = detector.detect(r1, r2, "", i1, i2);
      CHECK(res.assay == "sc-rna");
      CHECK(res.confidence == "high (poly-A/T tail signature)");
    }
  }

  SUBCASE("Detect DNA (test_1)") {
    std::string r1 = data_dir + "test_1.fastq.gz";
    if (std::filesystem::exists(r1)) {
      AssayDetector::DetectionResult res = detector.detect(r1, "", "", "", "");
      // test_1 is a small DNA sample, might be detected as dna with low/medium
      // confidence
      CHECK(res.assay == "dna");
    }
  }
}
} // namespace

// End suppression region
// NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
