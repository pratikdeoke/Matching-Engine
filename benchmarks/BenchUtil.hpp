// Latency measurement utilities.
//
// Two things this gets right that naive benchmarks usually do not:
//
// 1. Percentiles, not just the mean. A matching engine's mean latency is
//    uninteresting — the tail is what determines whether a venue is usable. We
//    record every sample and report p50/p90/p99/p99.9/max.
//
// 2. Honest timing. We measure with a single clock read before and after each
//    command. That read itself costs something (roughly 20-25 ns per
//    clock_gettime pair on typical x86 Linux via the vDSO), which is a
//    meaningful fraction of a sub-microsecond operation. `clock_overhead_ns()`
//    measures it so the reported numbers can be interpreted honestly rather
//    than silently including timer cost.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace te::bench {

using Clock = std::chrono::steady_clock;

[[nodiscard]] inline std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             Clock::now().time_since_epoch())
      .count();
}

// Estimates the cost of the timing instrumentation itself.
[[nodiscard]] inline double clock_overhead_ns(std::size_t iters = 200000) {
  // Warm up so the vDSO page and branch predictors are hot.
  for (std::size_t i = 0; i < 1000; ++i) {
    (void)now_ns();
  }
  const std::int64_t start = now_ns();
  std::int64_t sink = 0;
  for (std::size_t i = 0; i < iters; ++i) {
    sink += now_ns();
  }
  const std::int64_t end = now_ns();
  // Keep `sink` observable so the loop is not optimised away.
  asm volatile("" : : "r"(sink) : "memory");
  return static_cast<double>(end - start) / static_cast<double>(iters);
}

struct Stats {
  std::size_t count{0};
  double mean{0};
  std::int64_t min{0};
  std::int64_t p50{0};
  std::int64_t p90{0};
  std::int64_t p99{0};
  std::int64_t p999{0};
  std::int64_t max{0};
};

// Takes the sample vector by value and sorts it: percentile extraction needs
// order statistics, and mutating the caller's data would be surprising.
[[nodiscard]] inline Stats summarize(std::vector<std::int64_t> samples) {
  Stats s;
  if (samples.empty()) {
    return s;
  }
  std::sort(samples.begin(), samples.end());
  s.count = samples.size();

  long double sum = 0;
  for (std::int64_t v : samples) {
    sum += static_cast<long double>(v);
  }
  s.mean = static_cast<double>(sum / static_cast<long double>(samples.size()));

  auto pct = [&samples](double p) {
    // Nearest-rank percentile: index = ceil(p/100 * N) - 1, clamped.
    const auto n = static_cast<double>(samples.size());
    auto idx = static_cast<std::size_t>(std::ceil(p / 100.0 * n));
    if (idx > 0) {
      --idx;
    }
    return samples[std::min(idx, samples.size() - 1)];
  };

  s.min = samples.front();
  s.max = samples.back();
  s.p50 = pct(50.0);
  s.p90 = pct(90.0);
  s.p99 = pct(99.0);
  s.p999 = pct(99.9);
  return s;
}

inline void print_header() {
  std::printf("%-34s %10s %9s %8s %8s %8s %9s %9s\n", "benchmark", "samples",
              "mean_ns", "p50", "p90", "p99", "p99.9", "max");
  std::printf("%s\n", std::string(103, '-').c_str());
}

inline void print_row(const std::string &name, const Stats &s) {
  std::printf("%-34s %10zu %9.1f %8lld %8lld %8lld %9lld %9lld\n", name.c_str(),
              s.count, s.mean, static_cast<long long>(s.p50),
              static_cast<long long>(s.p90), static_cast<long long>(s.p99),
              static_cast<long long>(s.p999), static_cast<long long>(s.max));
}

inline void print_throughput(const std::string &name, std::size_t ops,
                             std::int64_t elapsed_ns) {
  const double secs = static_cast<double>(elapsed_ns) / 1e9;
  const double per_sec = secs > 0 ? static_cast<double>(ops) / secs : 0.0;
  std::printf("%-34s %10zu ops in %8.3f ms  =>  %10.0f ops/sec  (%6.1f ns/op)\n",
              name.c_str(), ops, secs * 1e3, per_sec,
              static_cast<double>(elapsed_ns) / static_cast<double>(ops));
}

} // namespace te::bench
