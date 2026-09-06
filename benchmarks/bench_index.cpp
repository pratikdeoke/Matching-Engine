// A/B micro-benchmark: FlatHashMap vs std::unordered_map on the exact access
// pattern the order index sees.
//
// This exists to justify a design decision with numbers rather than folklore.
// Replacing std::unordered_map was motivated by an observed periodic latency
// spike in the engine insert benchmark; this isolates the container so the
// comparison is not confounded by book or matching work.
//
// Pattern modelled: insert an id, look it up a few times, erase it, with a
// steady-state population of live entries — which is what a session of
// new-order / cancel flow does to the index.
//
// Each configuration is run several times and every run is printed, because a
// single run on a shared or virtualised host is not evidence.
#include "BenchUtil.hpp"

#include "core/Types.hpp"
#include "util/FlatHashMap.hpp"

#include <cstdio>
#include <unordered_map>
#include <vector>

using namespace te;
using namespace te::bench;

namespace {

struct Dummy {
  std::int64_t a{}, b{};
};

// Deterministic pseudo-random permutation of erase order, shared by both
// containers so they do the identical amount of work.
std::vector<std::uint64_t> make_ids(std::size_t n, std::uint64_t seed) {
  std::vector<std::uint64_t> ids;
  ids.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    ids.push_back(static_cast<std::uint64_t>(i) + 1);
  }
  // xorshift-based Fisher-Yates so both containers see the same order.
  std::uint64_t s = seed | 1;
  auto next = [&s] {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  };
  for (std::size_t i = ids.size(); i > 1; --i) {
    std::swap(ids[i - 1], ids[next() % i]);
  }
  return ids;
}


// FlatHashMap specialisation.
std::int64_t run_flat(std::size_t n, std::size_t live_target, bool prereserve,
                      std::int64_t *checksum) {
  FlatHashMap<OrderId, Dummy *> m;
  if (prereserve) {
    m.reserve(live_target * 2);
  }
  const auto ids = make_ids(n, 12345);
  std::vector<Dummy> backing(n);
  // FIFO of live ids as a ring buffer. An earlier version used
  // vector::erase(begin()), whose O(n) memmove over 50k elements completely
  // dominated the container cost being measured — the harness was the
  // benchmark. Retaining the note because it is the classic way these
  // comparisons go wrong.
  std::vector<std::uint64_t> live(live_target + 1);
  std::size_t head = 0, tail = 0, count = 0;
  const std::size_t cap = live.size();
  std::int64_t sum = 0;

  const std::int64_t t0 = now_ns();
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint64_t id = ids[i];
    m.insert_or_assign(OrderId{id}, &backing[i]);
    live[tail] = id;
    tail = (tail + 1) % cap;
    ++count;
    // Two lookups per insert: duplicate-id rejection plus a later cancel or
    // fill referencing the same order.
    if (Dummy **p = m.find(OrderId{id}); p != nullptr) {
      sum += (*p)->a;
    }
    if (Dummy **p = m.find(OrderId{live[(head + count / 2) % cap]});
        p != nullptr) {
      sum += (*p)->b;
    }
    if (count > live_target) {
      m.erase(OrderId{live[head]});
      head = (head + 1) % cap;
      --count;
    }
  }
  const std::int64_t dt = now_ns() - t0;
  *checksum = sum + static_cast<std::int64_t>(m.size());
  return dt;
}

std::int64_t run_std(std::size_t n, std::size_t live_target, bool prereserve,
                     std::int64_t *checksum) {
  std::unordered_map<OrderId, Dummy *> m;
  if (prereserve) {
    m.reserve(live_target * 2);
  }
  const auto ids = make_ids(n, 12345);
  std::vector<Dummy> backing(n);
  std::vector<std::uint64_t> live(live_target + 1);
  std::size_t head = 0, tail = 0, count = 0;
  const std::size_t cap = live.size();
  std::int64_t sum = 0;

  const std::int64_t t0 = now_ns();
  for (std::size_t i = 0; i < n; ++i) {
    const std::uint64_t id = ids[i];
    m.insert_or_assign(OrderId{id}, &backing[i]);
    live[tail] = id;
    tail = (tail + 1) % cap;
    ++count;
    if (auto it = m.find(OrderId{id}); it != m.end()) {
      sum += it->second->a;
    }
    if (auto it = m.find(OrderId{live[(head + count / 2) % cap]});
        it != m.end()) {
      sum += it->second->b;
    }
    if (count > live_target) {
      m.erase(OrderId{live[head]});
      head = (head + 1) % cap;
      --count;
    }
  }
  const std::int64_t dt = now_ns() - t0;
  *checksum = sum + static_cast<std::int64_t>(m.size());
  return dt;
}

// Per-operation latency distribution for the insert step alone. This is where
// the node allocation shows up, so it is the comparison that matters.
Stats insert_latency_flat(std::size_t n, bool prereserve) {
  FlatHashMap<OrderId, Dummy *> m;
  if (prereserve) {
    m.reserve(n);
  }
  std::vector<Dummy> backing(n);
  const auto ids = make_ids(n, 999);
  std::vector<std::int64_t> s;
  s.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const std::int64_t t0 = now_ns();
    m.insert_or_assign(OrderId{ids[i]}, &backing[i]);
    s.push_back(now_ns() - t0);
  }
  return summarize(std::move(s));
}

Stats insert_latency_std(std::size_t n, bool prereserve) {
  std::unordered_map<OrderId, Dummy *> m;
  if (prereserve) {
    m.reserve(n);
  }
  std::vector<Dummy> backing(n);
  const auto ids = make_ids(n, 999);
  std::vector<std::int64_t> s;
  s.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const std::int64_t t0 = now_ns();
    m.insert_or_assign(OrderId{ids[i]}, &backing[i]);
    s.push_back(now_ns() - t0);
  }
  return summarize(std::move(s));
}

} // namespace

int main() {
  constexpr std::size_t kN = 500'000;
  constexpr std::size_t kLive = 50'000;
  constexpr int kRuns = 5;

  std::printf("order-index A/B  (n=%zu ops, steady-state live=%zu, %d runs "
              "each)\n\n",
              kN, kLive, kRuns);

  // Warm both paths before measuring either.
  {
    std::int64_t c = 0;
    (void)run_flat(50'000, 5'000, true, &c);
    (void)run_std(50'000, 5'000, true, &c);
  }

  std::printf("%-28s %10s %12s\n", "container", "run", "ns/op");
  std::printf("%s\n", std::string(52, '-').c_str());
  for (int r = 0; r < kRuns; ++r) {
    std::int64_t c = 0;
    const std::int64_t dt = run_std(kN, kLive, true, &c);
    std::printf("%-28s %10d %12.1f\n", "std::unordered_map", r,
                static_cast<double>(dt) / static_cast<double>(kN));
  }
  for (int r = 0; r < kRuns; ++r) {
    std::int64_t c = 0;
    const std::int64_t dt = run_flat(kN, kLive, true, &c);
    std::printf("%-28s %10d %12.1f\n", "FlatHashMap", r,
                static_cast<double>(dt) / static_cast<double>(kN));
  }

  std::printf("\ninsert latency distribution, no pre-reservation (this is the "
              "allocation effect)\n\n");
  print_header();
  print_row("std::unordered_map insert", insert_latency_std(200'000, false));
  print_row("FlatHashMap insert", insert_latency_flat(200'000, false));

  std::printf("\ninsert latency distribution, pre-reserved\n\n");
  print_header();
  print_row("std::unordered_map insert", insert_latency_std(200'000, true));
  print_row("FlatHashMap insert", insert_latency_flat(200'000, true));
  return 0;
}
