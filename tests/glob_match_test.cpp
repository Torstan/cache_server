#include "test_harness.h"

#include <string>
#include <string_view>

#include "common/glob_match.h"

CACHE_TEST(GlobMatchSupportsRedisWildcards) {
  test::Require(common::GlobMatch("*", "abc"), "star matches all text");
  test::Require(common::GlobMatch("a*c", "abc"), "star matches middle text");
  test::Require(common::GlobMatch("a?c", "abc"), "question matches one byte");
  test::Require(!common::GlobMatch("a?c", "abbc"),
                "question does not match two bytes");
}

CACHE_TEST(GlobMatchSupportsCharacterClasses) {
  test::Require(common::GlobMatch("h[ae]llo", "hello"),
                "class matches first alternative");
  test::Require(common::GlobMatch("h[ae]llo", "hallo"),
                "class matches second alternative");
  test::Require(!common::GlobMatch("h[ae]llo", "hollo"),
                "class rejects missing alternative");
  test::Require(common::GlobMatch("item[0-9]", "item7"),
                "class range matches digit");
  test::Require(!common::GlobMatch("item[^0-9]", "item7"),
                "negated class rejects digit");
  test::Require(common::GlobMatch("item[^0-9]", "itemx"),
                "negated class accepts non-digit");
  test::Require(common::GlobMatch("item[!a]", "item!"),
                "exclamation mark is a class literal");
  test::Require(common::GlobMatch("item[!a]", "itema"),
                "literal exclamation class also matches other members");
  test::Require(!common::GlobMatch("item[!a]", "itemx"),
                "literal exclamation class rejects non-members");
  test::Require(common::GlobMatch("item[z-a]", "itemm"),
                "reversed range matches normalized range");
  test::Require(common::GlobMatch("[0-]", ":"),
                "range ending at closed bracket matches intervening byte");
}

CACHE_TEST(GlobMatchSupportsEscapesAndBinaryStrings) {
  test::Require(common::GlobMatch(R"(a\*b)", "a*b"),
                "escaped star is literal");
  test::Require(common::GlobMatch(R"(a\?b)", "a?b"),
                "escaped question mark is literal");
  test::Require(common::GlobMatch(R"([\--0])", "-"),
                "escaped dash is a literal class member");
  test::Require(common::GlobMatch(R"([\--0])", "0"),
                "class with escaped dash matches later literal member");
  test::Require(!common::GlobMatch(R"([\--0])", "."),
                "escaped dash does not start a range");
  test::Require(!common::GlobMatch(R"([\--0])", "/"),
                "escaped dash does not range through slash");
  test::Require(common::GlobMatch(R"([\^a])", "^"),
                "escaped caret is a literal class member");
  const std::string value(std::string("a", 1) + '\0' + "b");
  test::Require(common::GlobMatch(std::string_view("a?b", 3), value),
                "matcher is binary-safe for question mark");
  test::Require(!common::GlobMatch(std::string_view("a\\0b", 4), value),
                "backslash zero does not invent a NUL byte");
}

CACHE_TEST(GlobMatchHandlesScanMatchEdgeCases) {
  test::Require(common::GlobMatch("", ""), "empty pattern matches empty value");
  test::Require(!common::GlobMatch("", "x"),
                "empty pattern rejects non-empty value");
  test::Require(!common::GlobMatch("?", ""),
                "single-byte wildcard rejects empty value");
  test::Require(common::GlobMatch("a**c", "abc"),
                "consecutive stars match as one star");
  test::Require(common::GlobMatch("*a*ab", "aaab"),
                "star backtracking finds later literal suffix");
  test::Require(common::GlobMatch(R"(abc\)", R"(abc\)"),
                "trailing backslash is literal");
  test::Require(!common::GlobMatch("[ab", "a"),
                "unclosed class rejects matching member");
  test::Require(!common::GlobMatch("[]", "]"),
                "empty class is malformed and rejects");
  test::Require(common::GlobMatch("[]]", "]"),
                "closed class can include literal closing bracket first");
  test::Require(!common::GlobMatch("[^]", "x"),
                "negated empty class is malformed and rejects");
}
