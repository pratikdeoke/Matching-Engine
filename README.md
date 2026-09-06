# Matching Engine

A single-process, deterministic **limit order matching engine** written in modern C++20 — the core algorithmic component of an exchange, built as a standalone library with a strong emphasis on correctness, cache-friendly data structures, and low, predictable latency.

This is not a full trading platform: there is no network gateway, no persistence layer, no UI. It is the matching core itself — the piece you'd build first, then wrap in a transport layer to turn into an actual venue.

## What it does

Given a stream of order commands (new / modify / cancel), the engine:

- Maintains a **limit order book** per instrument with strict **price-time priority**
- Matches crossing orders and emits trade events at the resting order's price
- Supports **Limit** and **Market** orders, with **Day / IOC / FOK** time-in-force
- Prevents a participant from trading against their own resting orders
- Validates every order against **pre-trade risk limits** before it touches the book
- Journals every command so a run can be **replayed byte-for-byte identically**
- Routes multiple instruments through a single **exchange shard**

## Why it's built this way

A few deliberate design decisions drive the whole codebase, because they're the kind of thing an interviewer will ask about:

| Decision | Why |
|---|---|
| **Prices are signed integer ticks, never `double`** | Floating point comparisons introduce platform/compiler-dependent rounding, which would make matching non-deterministic. Integers give exact equality/ordering and match how real exchange protocols represent price. |
| **Order book sides are sorted `std::vector<PriceLevel*>`, not `std::map`** | The matcher only ever touches the best price. `vector::back()` is one dereference into contiguous memory; `std::map::begin()` chases a red-black tree node scattered across the heap. Consuming the best level is `pop_back()` — no allocation, no rebalancing. |
| **Resting orders live in an object pool, not individually `new`'d** | The hot path never calls the general-purpose allocator. Order and price-level memory is pre-chunked and reused, keeping allocation cost and latency variance out of matching. |
| **A hand-written flat hash map for order lookup** | `std::unordered_map` isn't cache-friendly enough for a lookup that fires on every command. `FlatHashMap` uses open addressing with SplitMix64-based hashing of the strongly-typed IDs. |
| **Trades print at the resting order's price** | The order that was already waiting set the terms; the aggressor gets price improvement if they crossed further than necessary. This also guarantees the book can never end up crossed after a sweep. |
| **Self-match prevention cancels the resting order, not the aggressor** | The common cause of a self-match is a stale quote left by the same participant. Cancelling the resting order preserves the intent of the newer command instead of blocking a participant from trading at that price indefinitely. |
| **FOK is evaluated before any state change** | The engine computes the fillable quantity first (honoring self-match exclusions) and rejects the whole order if it can't be filled completely — so FOK never produces a partial fill or a visible intermediate book state. |
| **Modify: price change or quantity increase loses time priority; quantity decrease keeps it** | Priority is a scarce resource. Letting someone jump the queue by amending an old order upward would break the fairness the whole book design depends on. |
| **The journal records commands, not book snapshots** | Because the engine is fully deterministic (integer prices, sequence-number time priority, no wall-clock or randomness in any decision), replaying the same command sequence into a fresh exchange reproduces the identical book state and event stream — from one small artefact. |
| **`Exchange` is single-threaded by design** | The intended concurrency model is multiple shards, each single-threaded and owning a disjoint set of instruments — the standard exchange architecture. This avoids a global lock around matching that would serialize the whole venue on its slowest instrument. |
| **The synthetic workload generator uses a self-contained xoshiro256\*\* RNG, not `std::mt19937`** | `std::uniform_int_distribution`'s output isn't specified across standard library implementations, so "same seed → same workload" would silently break across toolchains. A fully specified RNG keeps benchmarks and property tests reproducible anywhere. |

## Project structure

```
.
├── CMakeLists.txt
├── include/
│   ├── core/            # Types, strong IDs (OrderId, ClientId, ...), Order, Instrument, Commands
│   ├── events/          # Event types the engine emits (fills, trades, cancels, rejects)
│   ├── orderbook/        # Per-instrument limit order book (BookSide, PriceLevel)
│   ├── matching/          # The matching engine itself
│   ├── exchange/          # Multi-instrument exchange shard
│   ├── marketdata/        # Depth / trade publishing
│   ├── risk/               # Pre-trade risk validation (header-only)
│   ├── replay/             # Command journal + replay driver
│   ├── tools/               # Deterministic synthetic workload generator
│   └── util/                # FlatHashMap, ObjectPool
├── src/                    # Implementation files matching the headers above
├── tests/                  # Unit + property-based test suite
└── benchmarks/            # Latency (percentile) and throughput benchmarks
```

## Building

### Prerequisites
- CMake ≥ 3.16
- A C++20 compiler (GCC ≥ 10, Clang ≥ 12, or MSVC with a MinGW/Clang toolchain — this project has not been tested against pure MSVC, and its GCC/Clang-specific warning flags will not compile there)
- Optional: [GoogleTest](https://github.com/google/googletest) installed locally (`libgtest-dev` on Debian/Ubuntu). If it isn't found, CMake attempts to fetch it; if that also fails (no network), the suite falls back to a small built-in GoogleTest-API-compatible shim (`tests/TestHarness.hpp`), so `ctest` still works offline.
- Optional: [Google Benchmark](https://github.com/google/benchmark) for the JSON/statistics-driven benchmark target (`libbenchmark-dev`). Without it, the two standalone benchmark binaries still build and run.

### Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Leaving `CMAKE_BUILD_TYPE` unset defaults to `Release` automatically — an unoptimized build would make the latency numbers meaningless, so the project enforces this itself.

### Run the tests

```bash
ctest --test-dir build --output-on-failure
```

The suite covers order book invariants, matching semantics (self-match prevention, FOK, sweeps across levels), full order lifecycle, the custom hash map (including a differential test against `std::unordered_map` under randomized operations), and property-based checks: the book never crosses, quantity is conserved, no order outlives its terminal state, and the object pool returns memory as orders leave the book.

### Run the benchmarks

```bash
./build/te_bench            # full run
./build/te_bench --quick    # smaller sample size, fast sanity check
./build/te_bench_index      # insertion latency vs. book depth
```

Sample output (Release build, single core):

```
benchmark                             samples   mean_ns      p50      p90      p99     p99.9       max
-------------------------------------------------------------------------------------------------------
new_limit_passive                       20000     392.6      237      320      496       991   1480421
cancel_resting                          20000     419.1      382      568      898      1312    111123
modify_reprice                          20000     358.9      342      487      767      1026     33190
match_full_fill_1_trade                 20000     184.5      176      187      249       456     25239
match_partial_resting                   20000     245.1      217      300      488      1099    255108
match_sweep_5_levels                     5000     442.3      435      446      653       985      1628
match_sweep_20_levels                    2000    1643.8     1338     2135     3887     36703    175563
mixed_flow_all_commands                 50000     245.0      218      343      580       881     76163

insert_only    50000 ops in   39.709 ms  =>  1,259,164 ops/sec
mixed_flow     50000 ops in   11.366 ms  =>  4,399,040 ops/sec
```

Latency numbers naturally vary by machine and OS scheduling noise — the `p99.9`/`max` columns exist specifically to make that tail visible rather than hidden behind a mean.

### Useful CMake options

| Option | Default | Purpose |
|---|---|---|
| `TE_BUILD_TESTS` | `ON` | Build the test suite |
| `TE_BUILD_BENCHMARKS` | `ON` | Build the benchmark binaries |
| `TE_ENABLE_SANITIZERS` | `OFF` | Build with AddressSanitizer + UndefinedBehaviorSanitizer |
| `TE_ENABLE_WERROR` | `OFF` | Treat warnings as errors |
| `TE_ENABLE_NATIVE` | `OFF` | Build with `-march=native` (don't ship binaries built this way) |

Example:
```bash
cmake -S . -B build-asan -DTE_ENABLE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
```

## Design notes worth reading in the source

The header comments in this codebase double as a design rationale document — in particular:
- `include/orderbook/OrderBook.hpp` — why the book is a sorted vector of pointers, not a map or a vector of values
- `include/matching/MatchingEngine.hpp` — the full semantics for priority, trade pricing, market orders, TIF, self-match prevention, and modify
- `include/replay/EventLog.hpp` — the journal format and why determinism makes replay possible
- `include/exchange/Exchange.hpp` — the sharding model and why it's single-threaded per shard

## Known limitations

- No network/gateway layer — commands are submitted in-process via the `Exchange` API, not over a wire protocol
- No persistence beyond the flat-file command journal
- Single-threaded per shard by design (see above) — no built-in multi-shard orchestration