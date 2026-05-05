#include "../support/doctest.h"
#include "dna_utils.h"
#include "parse_utils.h"

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

} // namespace