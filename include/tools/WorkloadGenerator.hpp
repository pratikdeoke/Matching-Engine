// Synthetic order-flow generator.
//
// Produces a reproducible command stream for benchmarks, property tests, and
// replay fixtures. Reproducibility is the whole point: the RNG is a self-
// contained xoshiro256** seeded explicitly, not std::mt19937 with a
// library-dependent distribution implementation. std::uniform_int_distribution
// is deliberately avoided because its output is not specified across standard
// library versions, which would make "same seed => same workload" false when the
// toolchain changes.
//
// The flow model is intentionally simple but not uniform-random: prices are
// drawn around a per-symbol reference that random-walks, so the book develops
// real depth and the matcher sees realistic sweep depths instead of either
// never crossing or crossing everything.
#pragma once

#include "core/Commands.hpp"
#include "core/Types.hpp"

#include <cstdint>
#include <vector>

namespace te {

// xoshiro256** — small, fast, well-distributed, and fully specified here so the
// generated stream depends only on the seed.
class Rng {
public:
  explicit Rng(std::uint64_t seed) { reseed(seed); }

  void reseed(std::uint64_t seed) {
    // splitmix64 to expand a single seed into the four-word state.
    for (std::uint64_t &w : s_) {
      seed += 0x9e3779b97f4a7c15ULL;
      std::uint64_t z = seed;
      z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
      z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
      w = z ^ (z >> 31);
    }
  }

  std::uint64_t next() {
    const std::uint64_t result = rotl(s_[1] * 5, 7) * 9;
    const std::uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }

  // Unbiased bounded draw in [0, n) via Lemire's method with rejection.
  std::uint64_t below(std::uint64_t n) {
    if (n == 0) {
      return 0;
    }
    const std::uint64_t threshold = -n % n;
    for (;;) {
      const std::uint64_t r = next();
      if (r >= threshold) {
        return r % n;
      }
    }
  }

  // Inclusive integer range.
  std::int64_t range(std::int64_t lo, std::int64_t hi) {
    if (hi <= lo) {
      return lo;
    }
    return lo + static_cast<std::int64_t>(
                    below(static_cast<std::uint64_t>(hi - lo + 1)));
  }

  // Percent chance, 0..100.
  bool chance(std::uint32_t percent) {
    return below(100) < static_cast<std::uint64_t>(percent);
  }

private:
  static std::uint64_t rotl(std::uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
  }
  std::uint64_t s_[4]{};
};

struct WorkloadConfig {
  std::uint64_t seed{42};
  std::size_t num_commands{100'000};
  std::size_t num_clients{16};

  // Mix, in percent. new_order takes the remainder.
  std::uint32_t pct_cancel{20};
  std::uint32_t pct_modify{5};

  // Within new orders, in percent.
  std::uint32_t pct_market{3};
  std::uint32_t pct_ioc{7};
  // Chance a limit order is priced to cross the spread (an aggressor) rather
  // than joining/improving the book.
  std::uint32_t pct_aggressive{30};

  Ticks ref_price{10'000};      // starting reference, in ticks
  Ticks price_band{50};         // passive orders are placed within this band
  Ticks tick_size{1};
  Qty min_qty{1};
  Qty max_qty{500};
  Qty lot_size{1};
};

// Generates commands for a fixed set of symbols. Symbols are supplied as ids so
// the generator does not need the instrument table.
class WorkloadGenerator {
public:
  WorkloadGenerator(WorkloadConfig cfg, std::vector<SymbolId> symbols);

  // Produces the full command vector. Generated ahead of time rather than
  // streamed so that a benchmark measures only engine time, with no generator
  // work interleaved in the timed region.
  [[nodiscard]] std::vector<Command> generate();

  // Regenerates the identical stream (same seed, same symbols).
  void reset();

private:
  Command next_command();
  Command make_new_order(SymbolId sym);

  // Tracks ids the generator believes are live, so cancels and modifies mostly
  // target real orders instead of being uniformly rejected. It is only an
  // approximation — the generator does not run the matcher, so some targets will
  // already be filled. That is intentional: a realistic stream contains some
  // "unknown order id" rejects.
  struct LiveOrder {
    OrderId id;
    ClientId client;
    SymbolId symbol;
  };

  WorkloadConfig cfg_;
  std::vector<SymbolId> symbols_;
  Rng rng_;
  std::uint64_t next_order_id_{1};
  std::vector<LiveOrder> live_;
  std::vector<Ticks> refs_; // per-symbol reference price, random-walking
};

} // namespace te
