cmake_minimum_required(VERSION 3.16)
project(trading_engine LANGUAGES CXX VERSION 0.1.0)

# ---------------------------------------------------------------------------
# Standard and defaults
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # for clangd / clang-tidy

# A build with no CMAKE_BUILD_TYPE silently gets no optimisation, which for this
# project means benchmark numbers that are wrong by an order of magnitude.
# Default to Release and say so.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  message(STATUS "CMAKE_BUILD_TYPE not set; defaulting to Release")
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g3")
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -g -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")

option(TE_BUILD_TESTS "Build the test suite" ON)
option(TE_BUILD_BENCHMARKS "Build the benchmarks" ON)
option(TE_ENABLE_SANITIZERS "Build with AddressSanitizer + UBSan" OFF)
option(TE_ENABLE_WERROR "Treat warnings as errors" OFF)
option(TE_ENABLE_NATIVE "Build with -march=native (do not ship binaries built \
with this)" OFF)

# Warnings are part of the correctness story, so they are on everywhere rather
# than per-target.
add_library(te_warnings INTERFACE)
target_compile_options(te_warnings INTERFACE
  -Wall -Wextra -Wpedantic
  -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Woverloaded-virtual
  -Wcast-align -Wunused -Wdouble-promotion
  -Wformat=2 -Wimplicit-fallthrough)
if(TE_ENABLE_WERROR)
  target_compile_options(te_warnings INTERFACE -Werror)
endif()

# Sanitizers as a build option rather than a separate toolchain: the test suite
# is expected to pass clean under them, and that is how the instrument-table
# use-after-free in this codebase was found.
add_library(te_sanitizers INTERFACE)
if(TE_ENABLE_SANITIZERS)
  target_compile_options(te_sanitizers INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
  target_link_options(te_sanitizers INTERFACE -fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
# The engine is a plain static library with no dependency on any transport,
# database, or UI. That independence is a design requirement, and keeping the
# target's link list empty is how it stays enforced.
add_library(te_core STATIC
  src/core/Instrument.cpp
  src/orderbook/OrderBook.cpp
  src/matching/MatchingEngine.cpp
  src/marketdata/MarketData.cpp
  src/exchange/Exchange.cpp
  src/replay/EventLog.cpp
  src/tools/WorkloadGenerator.cpp)

target_include_directories(te_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(te_core PUBLIC te_warnings te_sanitizers)

if(TE_ENABLE_NATIVE)
  target_compile_options(te_core PUBLIC -march=native)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
# GoogleTest is used when present. When it is not, the suite falls back to a
# small GoogleTest-API-compatible shim (tests/TestHarness.hpp) so that `ctest`
# still works on a machine with no network access. The test sources are
# identical either way — no #ifdefs in the tests themselves.
if(TE_BUILD_TESTS)
  enable_testing()

  set(TE_TEST_SOURCES
    tests/test_main.cpp
    tests/test_order_book.cpp
    tests/test_matching.cpp
    tests/test_lifecycle.cpp
    tests/test_flat_hash_map.cpp
    tests/test_replay_property.cpp)

  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    # Try FetchContent, but do not make the build depend on the network
    # succeeding: fall back to the shim if it cannot be fetched.
    include(FetchContent)
    set(FETCHCONTENT_QUIET OFF)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.14.0
      GIT_SHALLOW TRUE)
    # Do not let GoogleTest override our compiler/linker settings on any platform.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_GetProperties(googletest)
    if(NOT googletest_POPULATED)
      # download_ok is false when there is no network; that is not an error.
      set(TE_GTEST_FETCH_OK TRUE)
      FetchContent_Populate(googletest
        QUIET
        SOURCE_DIR ${CMAKE_BINARY_DIR}/_deps/googletest-src
        BINARY_DIR ${CMAKE_BINARY_DIR}/_deps/googletest-build
        SUBBUILD_DIR ${CMAKE_BINARY_DIR}/_deps/googletest-subbuild)
      if(EXISTS ${CMAKE_BINARY_DIR}/_deps/googletest-src/CMakeLists.txt)
        add_subdirectory(${CMAKE_BINARY_DIR}/_deps/googletest-src
                         ${CMAKE_BINARY_DIR}/_deps/googletest-build EXCLUDE_FROM_ALL)
        set(TE_HAVE_GTEST TRUE)
      endif()
    endif()
  else()
    set(TE_HAVE_GTEST TRUE)
  endif()

  add_executable(te_tests ${TE_TEST_SOURCES})
  target_include_directories(te_tests PRIVATE tests)
  target_link_libraries(te_tests PRIVATE te_core)

  if(TE_HAVE_GTEST)
    if(TARGET GTest::gtest)
      target_link_libraries(te_tests PRIVATE GTest::gtest)
    else()
      target_link_libraries(te_tests PRIVATE gtest)
    endif()
    message(STATUS "Tests: using GoogleTest")
  else()
    target_compile_definitions(te_tests PRIVATE TE_USE_MINI_TEST)
    message(STATUS "Tests: GoogleTest unavailable, using built-in shim "
                   "(tests/TestHarness.hpp)")
  endif()

  add_test(NAME te_tests COMMAND te_tests)
  # The property/replay tests write journal files into the working directory.
  set_tests_properties(te_tests PROPERTIES
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    TIMEOUT 900)
endif()

# ---------------------------------------------------------------------------
# Benchmarks
# ---------------------------------------------------------------------------
if(TE_BUILD_BENCHMARKS)
  # The standalone harness has no third-party dependency and is the primary
  # measurement tool, because it reports percentiles rather than means.
  add_executable(te_bench benchmarks/bench_latency.cpp)
  target_include_directories(te_bench PRIVATE benchmarks)
  target_link_libraries(te_bench PRIVATE te_core)

  add_executable(te_bench_index benchmarks/bench_index.cpp)
  target_include_directories(te_bench_index PRIVATE benchmarks)
  target_link_libraries(te_bench_index PRIVATE te_core)

  # Google Benchmark is optional: it is only used for its statistics and JSON
  # regression-tracking output.
  find_package(benchmark QUIET)
  if(benchmark_FOUND)
    add_executable(te_gbench benchmarks/bench_gbench.cpp)
    target_link_libraries(te_gbench PRIVATE te_core benchmark::benchmark)
    message(STATUS "Benchmarks: Google Benchmark found, building te_gbench")
  else()
    message(STATUS "Benchmarks: Google Benchmark not found, skipping te_gbench "
                   "(te_bench and te_bench_index still build)")
  endif()

  if(TE_ENABLE_SANITIZERS)
    message(WARNING "Sanitizers are enabled: benchmark numbers from this build "
                    "are meaningless. Configure a separate Release build for "
                    "measurement.")
  endif()
endif()
