// Test runner entry point.
//
// With real GoogleTest, InitGoogleTest + RUN_ALL_TESTS. With the shim, the
// registry is walked directly.
#include "TestHarness.hpp"

#ifdef TE_USE_MINI_TEST
int main() { return ::mini::run_all(); }
#else
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif
