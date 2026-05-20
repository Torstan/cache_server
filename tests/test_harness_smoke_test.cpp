#include "test_harness.h"

CACHE_TEST(TestHarnessRunsRegisteredTests) {
  test::Require(true, "registered smoke test runs");
  test::RequireEqual("cache", "cache", "RequireEqual accepts equal strings");
}
