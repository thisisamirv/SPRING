#include "smoke_test_support.h"

using namespace smoke_test_support;

namespace {

TEST_CASE("Smoke paired reads plus single index lane round-trip") {
  SmokeWorkspace workspace("paired_with_i1_roundtrip");
  const auto archive = workspace.path("archive.sp");
  const auto output_prefix = workspace.path("tmp");

  run_spring({"-c", "--R1", asset_path("test_1.fastq"), "--R2",
              asset_path("test_2.fastq"), "--I1", asset_path("test_1.fastq"),
              "-o", archive});
  run_spring({"-d", "-i", archive, "-o", output_prefix});
  expect_text_match(workspace.path("tmp.R1"), asset_path("test_1.fastq"));
  expect_text_match(workspace.path("tmp.R2"), asset_path("test_2.fastq"));
  expect_text_match(workspace.path("tmp.I1"), asset_path("test_1.fastq"));
}

TEST_CASE("Smoke paired reads plus paired index lanes round-trip") {
  SmokeWorkspace workspace("paired_with_i1_i2_roundtrip");
  const auto archive = workspace.path("archive.sp");
  const auto output_prefix = workspace.path("tmp");

  run_spring({"-c", "--R1", asset_path("test_1.fastq"), "--R2",
              asset_path("test_2.fastq"), "--I1", asset_path("test_1.fastq"),
              "--I2", asset_path("test_2.fastq"), "-o", archive});
  run_spring({"-d", "-i", archive, "-o", output_prefix});
  expect_text_match(workspace.path("tmp.R1"), asset_path("test_1.fastq"));
  expect_text_match(workspace.path("tmp.R2"), asset_path("test_2.fastq"));
  expect_text_match(workspace.path("tmp.I1"), asset_path("test_1.fastq"));
  expect_text_match(workspace.path("tmp.I2"), asset_path("test_2.fastq"));
}

TEST_CASE("Smoke paired reads plus R3 round-trip") {
  SmokeWorkspace workspace("paired_with_r3_roundtrip");
  const auto archive = workspace.path("archive.sp");
  const auto output_prefix = workspace.path("tmp");

  run_spring({"-c", "--R1", asset_path("test_1.fastq"), "--R2",
              asset_path("test_2.fastq"), "--R3", asset_path("test_1.fastq"),
              "-o", archive});
  run_spring({"-d", "-i", archive, "-o", output_prefix});
  expect_text_match(workspace.path("tmp.R1"), asset_path("test_1.fastq"));
  expect_text_match(workspace.path("tmp.R2"), asset_path("test_2.fastq"));
  expect_text_match(workspace.path("tmp.R3"), asset_path("test_1.fastq"));
}

TEST_CASE("Smoke paired reads plus R3 and paired index lanes round-trip") {
  SmokeWorkspace workspace("paired_with_r3_i1_i2_roundtrip");
  const auto archive = workspace.path("archive.sp");
  const auto output_prefix = workspace.path("tmp");

  run_spring({"-c", "--R1", asset_path("test_1.fastq"), "--R2",
              asset_path("test_2.fastq"), "--R3", asset_path("test_1.fastq"),
              "--I1", asset_path("test_1.fastq"), "--I2",
              asset_path("test_2.fastq"), "-o", archive});
  run_spring({"-d", "-i", archive, "-o", output_prefix});
  expect_text_match(workspace.path("tmp.R1"), asset_path("test_1.fastq"));
  expect_text_match(workspace.path("tmp.R2"), asset_path("test_2.fastq"));
  expect_text_match(workspace.path("tmp.R3"), asset_path("test_1.fastq"));
  expect_text_match(workspace.path("tmp.I1"), asset_path("test_1.fastq"));
  expect_text_match(workspace.path("tmp.I2"), asset_path("test_2.fastq"));
}

} // namespace