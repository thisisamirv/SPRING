#include "../support/doctest.h"
#include "assay_detector.h"

#include <filesystem>
#include <string>

using namespace spring;

namespace {

TEST_CASE("Testing AssayDetector with Real Data") {
  AssayDetector detector;

  std::string base_path = ".";
  if (!std::filesystem::exists("data/samples") ||
      !std::filesystem::exists("data/reference")) {
    base_path = "..";
  }

  const std::string data_dir = base_path + "/data/samples/";
  const std::string ref_path =
      base_path + "/data/reference/ref_hg38_gencode49.fa";

  if (!std::filesystem::exists(ref_path)) {
    return;
  }

  SUBCASE("Detect Bisulfite (test_3)") {
    const std::string r1 = data_dir + "test_3_R1.fastq.gz";
    const std::string r2 = data_dir + "test_3_R2.fastq.gz";
    if (std::filesystem::exists(r1) && std::filesystem::exists(r2)) {
      AssayDetector::DetectionResult res = detector.detect(r1, r2, "", "", "");
      CHECK(res.assay == "bisulfite");
      CHECK(res.confidence.find("-depletion") != std::string::npos);
    }
  }

  SUBCASE("Detect sc-ATAC (test_4)") {
    const std::string r1 = data_dir + "test_4_R1.fastq.gz";
    const std::string r2 = data_dir + "test_4_R2.fastq.gz";
    const std::string r3 = data_dir + "test_4_R3.fastq.gz";
    const std::string i1 = data_dir + "test_4_I1.fastq.gz";
    if (std::filesystem::exists(r1)) {
      AssayDetector::DetectionResult res = detector.detect(r1, r2, r3, i1, "");
      CHECK(res.assay == "sc-atac");
      CHECK(res.confidence.find("Tn5 adapters") != std::string::npos);
    }
  }

  SUBCASE("Detect sc-RNA (test_5)") {
    const std::string r1 = data_dir + "test_5_R1.fastq.gz";
    const std::string r2 = data_dir + "test_5_R2.fastq.gz";
    const std::string i1 = data_dir + "test_5_I1.fastq.gz";
    const std::string i2 = data_dir + "test_5_I2.fastq.gz";
    if (std::filesystem::exists(r1)) {
      AssayDetector::DetectionResult res = detector.detect(r1, r2, "", i1, i2);
      CHECK(res.assay == "sc-rna");
      CHECK(res.confidence.find("poly-A/T tails") != std::string::npos);
    }
  }

  SUBCASE("Detect DNA (test_1)") {
    const std::string r1 = data_dir + "test_1.fastq.gz";
    if (std::filesystem::exists(r1)) {
      AssayDetector::DetectionResult res = detector.detect(r1, "", "", "", "");
      CHECK(res.assay == "dna");
    }
  }
}

} // namespace