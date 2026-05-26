#include "test_harness.h"
#include "common/parse_utils.h"

#include <cmath>
#include <cstdint>
#include <string>

CACHE_TEST(ParseInt64Valid) {
  std::int64_t v = 0;
  test::Require(common::ParseInt64("123", &v), "positive");
  test::Require(v == 123, "value 123");
  test::Require(common::ParseInt64("-456", &v), "negative");
  test::Require(v == -456, "value -456");
  test::Require(common::ParseInt64("0", &v), "zero");
  test::Require(v == 0, "value 0");
}

CACHE_TEST(ParseInt64Invalid) {
  std::int64_t v = 0;
  test::Require(!common::ParseInt64("", &v), "empty");
  test::Require(!common::ParseInt64("abc", &v), "alpha");
  test::Require(!common::ParseInt64("12.34", &v), "decimal");
  test::Require(!common::ParseInt64("123abc", &v), "trailing");
  test::Require(!common::ParseInt64(" 123", &v), "leading whitespace");
  test::Require(!common::ParseInt64("123 ", &v), "trailing whitespace");
}

CACHE_TEST(ParseFiniteDoubleValid) {
  double v = 0.0;
  test::Require(common::ParseFiniteDouble("3.14", &v), "decimal");
  test::Require(std::abs(v - 3.14) < 0.001, "value 3.14");
  test::Require(common::ParseFiniteDouble("-2.5", &v), "negative");
  test::Require(std::abs(v + 2.5) < 0.001, "value -2.5");
}

CACHE_TEST(ParseFiniteDoubleInvalid) {
  double v = 0.0;
  test::Require(!common::ParseFiniteDouble("", &v), "empty");
  test::Require(!common::ParseFiniteDouble("abc", &v), "alpha");
  test::Require(!common::ParseFiniteDouble("inf", &v), "inf");
  test::Require(!common::ParseFiniteDouble("nan", &v), "nan");
}

CACHE_TEST(ToUpperAscii) {
  test::RequireEqual(common::ToUpperAscii("hello"), std::string("HELLO"), "lower");
  test::RequireEqual(common::ToUpperAscii("WORLD"), std::string("WORLD"), "upper");
  test::RequireEqual(common::ToUpperAscii("set123"), std::string("SET123"), "mixed");
}
