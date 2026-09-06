// Minimal GoogleTest-compatible shim.
//
// The test files below are written against the GoogleTest API and normally link
// real GTest (CMake fetches it, or finds a system install). This shim implements
// the subset those tests use — TEST, TEST_F, EXPECT_*/ASSERT_* — so the suite can
// also be compiled and run in an environment with no GTest available, without
// maintaining two copies of the tests.
//
// Selected by -DTE_USE_MINI_TEST. When absent, <gtest/gtest.h> is used.
#pragma once

#ifndef TE_USE_MINI_TEST
#include <gtest/gtest.h>
#else

#include <cstdio>
#include <exception>
#include <string>
#include <type_traits>
#include <vector>

namespace mini {

struct TestCase {
  const char *suite;
  const char *name;
  void (*fn)();
};

inline std::vector<TestCase> &registry() {
  static std::vector<TestCase> r;
  return r;
}

// Per-test failure state. A failed EXPECT records and continues; a failed ASSERT
// throws to unwind out of the test body.
inline int &current_failures() {
  static int f = 0;
  return f;
}

struct AssertionAbort : std::exception {};

inline void report(const char *file, int line, const std::string &what) {
  ++current_failures();
  std::fprintf(stderr, "  %s:%d: FAILED: %s\n", file, line, what.c_str());
}

struct Registrar {
  Registrar(const char *suite, const char *name, void (*fn)()) {
    registry().push_back(TestCase{suite, name, fn});
  }
};

// Stringification that works for the types the tests compare. Strong ids and
// enums get printed via their .value / to_string where the tests need it, but
// for failure messages a numeric fallback is sufficient.
template <typename T> std::string str(const T &v) {
  if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<long long>(v));
  } else if constexpr (std::is_arithmetic_v<T>) {
    return std::to_string(v);
  } else if constexpr (std::is_convertible_v<T, std::string>) {
    return std::string(v);
  } else {
    return "<value>";
  }
}

inline int run_all() {
  int failed_tests = 0;
  int total = 0;
  for (const TestCase &tc : registry()) {
    ++total;
    current_failures() = 0;
    std::fprintf(stderr, "[ RUN      ] %s.%s\n", tc.suite, tc.name);
    try {
      tc.fn();
    } catch (const AssertionAbort &) {
      // already recorded
    } catch (const std::exception &e) {
      report("<exception>", 0, std::string("threw: ") + e.what());
    } catch (...) {
      report("<exception>", 0, "threw unknown exception");
    }
    if (current_failures() == 0) {
      std::fprintf(stderr, "[       OK ] %s.%s\n", tc.suite, tc.name);
    } else {
      std::fprintf(stderr, "[  FAILED  ] %s.%s\n", tc.suite, tc.name);
      ++failed_tests;
    }
  }
  std::fprintf(stderr, "\n%d test(s) run, %d failed.\n", total, failed_tests);
  return failed_tests == 0 ? 0 : 1;
}

} // namespace mini

// --- test definition macros ------------------------------------------------
#define TEST(suite, name)                                                      \
  static void suite##_##name();                                                \
  static ::mini::Registrar reg_##suite##_##name(#suite, #name,                 \
                                                &suite##_##name);              \
  static void suite##_##name()

// TEST_F: derive from the fixture, then run SetUp / body / TearDown.
#define TEST_F(fixture, name)                                                  \
  struct fixture##_##name : fixture {                                          \
    void body();                                                               \
  };                                                                           \
  static void run_##fixture##_##name() {                                       \
    fixture##_##name t;                                                        \
    t.SetUp();                                                                 \
    t.body();                                                                  \
    t.TearDown();                                                              \
  }                                                                            \
  static ::mini::Registrar reg_##fixture##_##name(#fixture, #name,             \
                                                  &run_##fixture##_##name);    \
  void fixture##_##name::body()

// --- assertion macros ------------------------------------------------------
#define TE_MINI_CHECK(cond, msg, fatal)                                        \
  do {                                                                         \
    if (!(cond)) {                                                             \
      ::mini::report(__FILE__, __LINE__, (msg));                               \
      if (fatal) {                                                             \
        throw ::mini::AssertionAbort{};                                        \
      }                                                                        \
    }                                                                          \
  } while (false)

#define EXPECT_TRUE(x) TE_MINI_CHECK((x), "EXPECT_TRUE(" #x ")", false)
#define EXPECT_FALSE(x) TE_MINI_CHECK(!(x), "EXPECT_FALSE(" #x ")", false)
#define ASSERT_TRUE(x) TE_MINI_CHECK((x), "ASSERT_TRUE(" #x ")", true)
#define ASSERT_FALSE(x) TE_MINI_CHECK(!(x), "ASSERT_FALSE(" #x ")", true)

#define EXPECT_EQ(a, b)                                                        \
  TE_MINI_CHECK((a) == (b), "EXPECT_EQ(" #a ", " #b ")", false)
#define EXPECT_NE(a, b)                                                        \
  TE_MINI_CHECK((a) != (b), "EXPECT_NE(" #a ", " #b ")", false)
#define EXPECT_LT(a, b) TE_MINI_CHECK((a) < (b), "EXPECT_LT(" #a ", " #b ")", false)
#define EXPECT_LE(a, b) TE_MINI_CHECK((a) <= (b), "EXPECT_LE(" #a ", " #b ")", false)
#define EXPECT_GT(a, b) TE_MINI_CHECK((a) > (b), "EXPECT_GT(" #a ", " #b ")", false)
#define EXPECT_GE(a, b) TE_MINI_CHECK((a) >= (b), "EXPECT_GE(" #a ", " #b ")", false)

#define ASSERT_EQ(a, b)                                                        \
  TE_MINI_CHECK((a) == (b), "ASSERT_EQ(" #a ", " #b ")", true)
#define ASSERT_NE(a, b)                                                        \
  TE_MINI_CHECK((a) != (b), "ASSERT_NE(" #a ", " #b ")", true)
#define ASSERT_GE(a, b) TE_MINI_CHECK((a) >= (b), "ASSERT_GE(" #a ", " #b ")", true)
#define ASSERT_GT(a, b) TE_MINI_CHECK((a) > (b), "ASSERT_GT(" #a ", " #b ")", true)
#define ASSERT_LT(a, b) TE_MINI_CHECK((a) < (b), "ASSERT_LT(" #a ", " #b ")", true)
#define ASSERT_LE(a, b) TE_MINI_CHECK((a) <= (b), "ASSERT_LE(" #a ", " #b ")", true)

#define EXPECT_STREQ(a, b)                                                     \
  TE_MINI_CHECK(std::string(a) == std::string(b),                              \
                "EXPECT_STREQ(" #a ", " #b ")", false)

// Fixture base, mirroring the GTest interface the tests use.
namespace testing {
class Test {
public:
  virtual ~Test() = default;
  virtual void SetUp() {}
  virtual void TearDown() {}
};
} // namespace testing

#endif // TE_USE_MINI_TEST
