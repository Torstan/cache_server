#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace test {

using TestFn = std::function<void()>;

struct TestCase {
  std::string name;
  TestFn fn;
};

void Register(std::string name, TestFn fn);
int RunAll();
void Require(bool condition, std::string_view message);
void RequireEqual(std::string_view actual, std::string_view expected,
                  std::string_view message);

}  // namespace test

#define CACHE_TEST(name)                                      \
  static void name();                                         \
  namespace {                                                 \
  struct name##_registrar {                                   \
    name##_registrar() { ::test::Register(#name, name); }     \
  } name##_registrar_instance;                                \
  }                                                           \
  static void name()
