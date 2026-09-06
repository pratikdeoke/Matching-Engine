// FlatHashMap tests.
//
// Backward-shift deletion is the part worth testing hard: it moves entries that
// the caller cannot see, and a mistake in the "can this entry move into the
// hole" predicate produces a map that silently loses keys only once probe
// chains start to collide. So the main test is differential — the same random
// operation stream is applied to FlatHashMap and std::unordered_map and the
// two are compared after every step.
#include "TestHarness.hpp"

#include "core/Types.hpp"
#include "util/FlatHashMap.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

using namespace te;

namespace {

// Hash that forces every key into the same bucket, so every operation exercises
// a maximally long probe chain and the backward-shift logic gets a real
// workout instead of the easy no-collision path.
struct CollidingHash {
  template <typename Key>
  std::size_t operator()(const Key &) const noexcept {
    return 0;
  }
};

std::uint64_t xs(std::uint64_t &s) {
  s ^= s << 13;
  s ^= s >> 7;
  s ^= s << 17;
  return s;
}

} // namespace

TEST(FlatHashMap, InsertFindErase) {
  FlatHashMap<OrderId, int> m;
  EXPECT_TRUE(m.empty());
  EXPECT_TRUE(m.insert_or_assign(OrderId{1}, 10));
  EXPECT_TRUE(m.insert_or_assign(OrderId{2}, 20));
  EXPECT_EQ(m.size(), 2u);

  ASSERT_TRUE(m.find(OrderId{1}) != nullptr);
  EXPECT_EQ(*m.find(OrderId{1}), 10);
  EXPECT_EQ(*m.find(OrderId{2}), 20);
  EXPECT_TRUE(m.find(OrderId{3}) == nullptr);

  // Overwrite returns false (no new entry) and does not change size.
  EXPECT_FALSE(m.insert_or_assign(OrderId{1}, 99));
  EXPECT_EQ(m.size(), 2u);
  EXPECT_EQ(*m.find(OrderId{1}), 99);

  EXPECT_TRUE(m.erase(OrderId{1}));
  EXPECT_FALSE(m.erase(OrderId{1})); // already gone
  EXPECT_EQ(m.size(), 1u);
  EXPECT_TRUE(m.find(OrderId{1}) == nullptr);
  EXPECT_EQ(*m.find(OrderId{2}), 20);
}

TEST(FlatHashMap, GrowsAndKeepsEveryEntry) {
  FlatHashMap<OrderId, std::uint64_t> m(8);
  constexpr std::uint64_t kN = 5000;
  for (std::uint64_t i = 1; i <= kN; ++i) {
    m.insert_or_assign(OrderId{i}, i * 3);
  }
  EXPECT_EQ(m.size(), kN);
  bool all_present = true;
  for (std::uint64_t i = 1; i <= kN; ++i) {
    const std::uint64_t *v = m.find(OrderId{i});
    if (v == nullptr || *v != i * 3) {
      all_present = false;
    }
  }
  EXPECT_TRUE(all_present);
  EXPECT_TRUE(m.find(OrderId{kN + 1}) == nullptr);
}

TEST(FlatHashMap, EraseDoesNotBreakProbeChains) {
  // Every key hashes to bucket 0, so all entries sit in one long chain and each
  // erase must shift the rest back correctly. With tombstones or a wrong shift
  // predicate, later lookups would stop at the hole and report missing keys.
  FlatHashMap<OrderId, int, CollidingHash> m(64);
  constexpr int kN = 40;
  for (int i = 1; i <= kN; ++i) {
    m.insert_or_assign(OrderId{static_cast<std::uint64_t>(i)}, i);
  }
  // Erase every third key, then verify the rest are all still reachable.
  for (int i = 1; i <= kN; i += 3) {
    EXPECT_TRUE(m.erase(OrderId{static_cast<std::uint64_t>(i)}));
  }
  bool ok = true;
  for (int i = 1; i <= kN; ++i) {
    const int *v = m.find(OrderId{static_cast<std::uint64_t>(i)});
    const bool should_exist = (i % 3) != 1;
    if (should_exist != (v != nullptr)) {
      ok = false;
    } else if (v != nullptr && *v != i) {
      ok = false;
    }
  }
  EXPECT_TRUE(ok);
}

TEST(FlatHashMap, ReinsertAfterEraseInCollidingChain) {
  FlatHashMap<OrderId, int, CollidingHash> m(32);
  for (int i = 1; i <= 20; ++i) {
    m.insert_or_assign(OrderId{static_cast<std::uint64_t>(i)}, i);
  }
  for (int i = 1; i <= 20; ++i) {
    m.erase(OrderId{static_cast<std::uint64_t>(i)});
  }
  EXPECT_EQ(m.size(), 0u);
  // Re-insert the same keys: with tombstone leakage this would either fail to
  // find them or force a spurious rehash.
  for (int i = 1; i <= 20; ++i) {
    EXPECT_TRUE(m.insert_or_assign(OrderId{static_cast<std::uint64_t>(i)}, i * 2));
  }
  EXPECT_EQ(m.size(), 20u);
  bool ok = true;
  for (int i = 1; i <= 20; ++i) {
    const int *v = m.find(OrderId{static_cast<std::uint64_t>(i)});
    if (v == nullptr || *v != i * 2) {
      ok = false;
    }
  }
  EXPECT_TRUE(ok);
}

TEST(FlatHashMap, MatchesUnorderedMapUnderRandomOperations) {
  // Differential test. Randomly insert, overwrite, erase, and look up, and
  // compare against the standard container after every operation.
  FlatHashMap<OrderId, std::uint64_t> flat(16);
  std::unordered_map<OrderId, std::uint64_t> ref;
  std::vector<std::uint64_t> known;
  std::uint64_t s = 0xC0FFEE;

  bool mismatch = false;
  for (int step = 0; step < 200000 && !mismatch; ++step) {
    const std::uint64_t r = xs(s);
    const int op = static_cast<int>(r % 100);
    // Keep the key space small relative to the operation count so collisions,
    // erases, and re-inserts of the same key all happen frequently.
    const std::uint64_t key = 1 + (xs(s) % 3000);

    if (op < 50) {
      const std::uint64_t val = xs(s);
      const bool a = flat.insert_or_assign(OrderId{key}, val);
      const bool b = ref.insert_or_assign(OrderId{key}, val).second;
      if (a != b) {
        mismatch = true;
      }
      known.push_back(key);
    } else if (op < 85) {
      const bool a = flat.erase(OrderId{key});
      const bool b = ref.erase(OrderId{key}) != 0;
      if (a != b) {
        mismatch = true;
      }
    } else {
      const std::uint64_t *a = flat.find(OrderId{key});
      auto it = ref.find(OrderId{key});
      const bool a_has = a != nullptr;
      const bool b_has = it != ref.end();
      if (a_has != b_has || (a_has && *a != it->second)) {
        mismatch = true;
      }
    }

    if (flat.size() != ref.size()) {
      mismatch = true;
    }
  }
  ASSERT_FALSE(mismatch);

  // Final full comparison in both directions.
  bool equal = flat.size() == ref.size();
  for (const auto &[k, v] : ref) {
    const std::uint64_t *f = flat.find(k);
    if (f == nullptr || *f != v) {
      equal = false;
    }
  }
  flat.for_each([&](OrderId k, const std::uint64_t &v) {
    auto it = ref.find(k);
    if (it == ref.end() || it->second != v) {
      equal = false;
    }
  });
  EXPECT_TRUE(equal);
}

TEST(FlatHashMap, ReserveAvoidsRehashDuringInserts) {
  FlatHashMap<OrderId, int> m;
  m.reserve(10000);
  const std::size_t cap = m.capacity();
  for (std::uint64_t i = 1; i <= 10000; ++i) {
    m.insert_or_assign(OrderId{i}, 1);
  }
  // The whole point of reserve() on the hot path: capacity must not have moved,
  // because moving it means an allocation happened mid-run.
  EXPECT_EQ(m.capacity(), cap);
  EXPECT_EQ(m.size(), 10000u);
}

TEST(FlatHashMap, ClearEmptiesWithoutLosingCapacity) {
  FlatHashMap<OrderId, int> m;
  m.reserve(1000);
  const std::size_t cap = m.capacity();
  for (std::uint64_t i = 1; i <= 500; ++i) {
    m.insert_or_assign(OrderId{i}, static_cast<int>(i));
  }
  m.clear();
  EXPECT_EQ(m.size(), 0u);
  EXPECT_EQ(m.capacity(), cap);
  EXPECT_TRUE(m.find(OrderId{250}) == nullptr);
  m.insert_or_assign(OrderId{7}, 7);
  EXPECT_EQ(m.size(), 1u);
  EXPECT_EQ(*m.find(OrderId{7}), 7);
}
