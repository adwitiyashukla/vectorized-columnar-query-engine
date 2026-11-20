#pragma once

#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace vcqe {

struct TestCase {
  std::string name;
  std::function<void()> body;
};

inline std::vector<TestCase>& Registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline int& Failures() {
  static int failures = 0;
  return failures;
}

struct Registrar {
  Registrar(const std::string& name, std::function<void()> body) {
    Registry().push_back(TestCase{name, std::move(body)});
  }
};

inline void Report(const char* file, int line, const std::string& message) {
  std::printf("  failed at %s:%d %s\n", file, line, message.c_str());
  ++Failures();
}

inline int RunAll() {
  int failed = 0;
  for (const TestCase& test : Registry()) {
    const int before = Failures();
    std::printf("%s\n", test.name.c_str());
    try {
      test.body();
    } catch (const std::exception& error) {
      Report(__FILE__, 0, std::string("threw ") + error.what());
    }
    if (Failures() > before) {
      ++failed;
    }
  }
  std::printf("%zu tests, %d failed\n", Registry().size(), failed);
  return failed == 0 ? 0 : 1;
}

}

#define TEST(name)                                                                                 \
  static void name();                                                                              \
  static vcqe::Registrar registrar_##name(#name, name);                                            \
  static void name()

#define EXPECT(condition)                                                                          \
  do {                                                                                             \
    if (!(condition)) {                                                                            \
      vcqe::Report(__FILE__, __LINE__, #condition);                                                \
    }                                                                                              \
  } while (false)

#define EXPECT_EQ(left, right)                                                                     \
  do {                                                                                             \
    const auto& expect_left = (left);                                                              \
    const auto& expect_right = (right);                                                            \
    if (!(expect_left == expect_right)) {                                                          \
      vcqe::Report(__FILE__, __LINE__, std::string(#left) + " != " + #right);                      \
    }                                                                                              \
  } while (false)

#define EXPECT_THROWS(statement)                                                                   \
  do {                                                                                             \
    bool threw = false;                                                                            \
    try {                                                                                          \
      statement;                                                                                   \
    } catch (const std::exception&) {                                                              \
      threw = true;                                                                                \
    }                                                                                              \
    if (!threw) {                                                                                  \
      vcqe::Report(__FILE__, __LINE__, std::string(#statement) + " did not throw");                \
    }                                                                                              \
  } while (false)

#define TEST_MAIN()                                                                                \
  int main() {                                                                                     \
    return vcqe::RunAll();                                                                         \
  }
