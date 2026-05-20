#include "test_harness.h"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace test {
namespace {

std::vector<TestCase>& Registry() {
  static std::vector<TestCase> registry;
  return registry;
}

}  // namespace

void Register(std::string name, TestFn fn) {
  Registry().push_back(TestCase{std::move(name), std::move(fn)});
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

void RequireEqual(std::string_view actual, std::string_view expected,
                  std::string_view message) {
  if (actual != expected) {
    throw std::runtime_error(std::string(message) + ": expected [" +
                             std::string(expected) + "], got [" +
                             std::string(actual) + "]");
  }
}

int RunAll() {
  int failed = 0;
  for (const TestCase& test_case : Registry()) {
    try {
      test_case.fn();
      std::cout << "[PASS] " << test_case.name << "\n";
    } catch (const std::exception& ex) {
      ++failed;
      std::cerr << "[FAIL] " << test_case.name << ": " << ex.what() << "\n";
    }
  }
  return failed == 0 ? 0 : 1;
}

}  // namespace test

int main() { return test::RunAll(); }
