// Open-addressing hash map with linear probing and backward-shift deletion.
//
// Why not std::unordered_map
// --------------------------
// The order index is on the hot path: every new order inserts, every cancel or
// fill erases, and every command looks up. std::unordered_map is node-based, so
// each insert calls the general allocator and each lookup chases a pointer into
// a separately-allocated node.
//
// That allocation is measurable, not theoretical. Benchmarking passive order
// insertion showed a latency spike on an exact 128-order period. libstdc++'s
// hash node for {OrderId, Order*} is 24 bytes, which malloc rounds into the
// 32-byte bin; 4096 / 32 = 128, so every 128th insert was the one that took a
// fresh 4 KiB page from the heap and paid a minor page fault for it. The
// engine's stated rule is no allocation on the hot path, and the order index was
// quietly breaking it.
//
// Design
//   - Power-of-two capacity, mask instead of modulo.
//   - Linear probing: probe sequences are contiguous, so a miss usually costs
//     one cache line rather than a pointer chase per step.
//   - A default-constructed Key is the empty sentinel. This works for the
//     strong id types in Types.hpp because id 0 is reserved as invalid, and it
//     keeps a slot at exactly sizeof(Key) + sizeof(Value) with no state byte.
//   - Backward-shift deletion instead of tombstones. Cancel-heavy order flow
//     erases constantly; tombstones would accumulate and force repeated
//     rehashing, which is the exact allocation this class exists to remove.
//   - reserve() allocates once. In steady state the map never allocates again.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace te {

// Fibonacci hashing: multiply by 2^64/phi and take the high bits. Sequential
// ids (which is what OrderIds are) spread across the table instead of landing
// in one contiguous run, without the cost of a stronger mixer.
struct IdHash {
  template <typename Key>
  [[nodiscard]] std::size_t operator()(const Key &k) const noexcept {
    return static_cast<std::size_t>(k.value * 0x9e3779b97f4a7c15ULL);
  }
};

template <typename Key, typename Value, typename Hash = IdHash>
class FlatHashMap {
public:
  explicit FlatHashMap(std::size_t initial_capacity = 64) {
    rehash(round_up_pow2(initial_capacity < 8 ? 8 : initial_capacity));
  }

  // Ensures the map can hold `n` entries without rehashing, honouring the load
  // factor. Call once at startup; after that the hot path never allocates.
  void reserve(std::size_t n) {
    const std::size_t need = round_up_pow2((n * 8) / 5 + 8); // 1/0.625 headroom
    if (need > slots_.size()) {
      rehash(need);
    }
  }

  // Returns a pointer to the stored value, or nullptr when absent. A pointer
  // rather than an iterator: callers only ever want the value, and it makes the
  // "not found" branch a null check.
  //
  // The pointer is invalidated by any insert that triggers a rehash, and by any
  // erase (backward-shift deletion moves entries). Callers must not hold it
  // across a mutation.
  [[nodiscard]] Value *find(const Key &key) noexcept {
    const std::size_t mask = slots_.size() - 1;
    std::size_t i = Hash{}(key) & mask;
    for (;;) {
      Slot &s = slots_[i];
      if (s.key == kEmpty) {
        return nullptr;
      }
      if (s.key == key) {
        return &s.value;
      }
      i = (i + 1) & mask;
    }
  }

  [[nodiscard]] const Value *find(const Key &key) const noexcept {
    return const_cast<FlatHashMap *>(this)->find(key);
  }

  [[nodiscard]] bool contains(const Key &key) const noexcept {
    return find(key) != nullptr;
  }

  // Inserts or overwrites. Returns true when a new entry was created.
  bool insert_or_assign(const Key &key, const Value &value) {
    if (Value *existing = find(key); existing != nullptr) {
      *existing = value;
      return false;
    }
    // Grow at a 0.625 load factor. Linear probing degrades sharply near full,
    // and the memory cost of extra slots is trivial next to a probe storm.
    if ((size_ + 1) * 8 >= slots_.size() * 5) {
      rehash(slots_.size() * 2);
    }
    const std::size_t mask = slots_.size() - 1;
    std::size_t i = Hash{}(key) & mask;
    while (slots_[i].key != kEmpty) {
      i = (i + 1) & mask;
    }
    slots_[i].key = key;
    slots_[i].value = value;
    ++size_;
    return true;
  }

  // Removes `key` if present. Returns true when something was removed.
  bool erase(const Key &key) noexcept {
    const std::size_t mask = slots_.size() - 1;
    std::size_t i = Hash{}(key) & mask;
    for (;;) {
      if (slots_[i].key == kEmpty) {
        return false;
      }
      if (slots_[i].key == key) {
        break;
      }
      i = (i + 1) & mask;
    }

    // Backward-shift deletion. Open a hole at i, then walk forward looking for
    // an entry whose ideal position is at or before the hole; move it in and
    // repeat with the hole at its old slot. Stops at the first empty slot,
    // which is where the probe chain ends. No tombstones are left behind.
    std::size_t hole = i;
    for (;;) {
      slots_[hole].key = kEmpty;
      std::size_t k = hole;
      for (;;) {
        k = (k + 1) & mask;
        if (slots_[k].key == kEmpty) {
          --size_;
          return true;
        }
        const std::size_t ideal = Hash{}(slots_[k].key) & mask;
        // Movable iff the hole lies within the cyclic range [ideal, k].
        if (((k - ideal) & mask) >= ((k - hole) & mask)) {
          break;
        }
      }
      slots_[hole] = slots_[k];
      hole = k;
    }
  }

  void clear() noexcept {
    for (Slot &s : slots_) {
      s.key = kEmpty;
    }
    size_ = 0;
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }

  // Visits every entry. Order is unspecified and changes across rehashes, so
  // callers that need determinism must sort. Used by diagnostics only.
  template <typename Fn> void for_each(Fn &&fn) const {
    for (const Slot &s : slots_) {
      if (!(s.key == kEmpty)) {
        fn(s.key, s.value);
      }
    }
  }

private:
  struct Slot {
    Key key{};
    Value value{};
  };

  static constexpr Key kEmpty{};

  static std::size_t round_up_pow2(std::size_t n) {
    std::size_t p = 1;
    while (p < n) {
      p <<= 1;
    }
    return p;
  }

  void rehash(std::size_t new_cap) {
    std::vector<Slot> old;
    old.swap(slots_);
    slots_.assign(new_cap, Slot{});
    const std::size_t mask = new_cap - 1;
    for (Slot &s : old) {
      if (s.key == kEmpty) {
        continue;
      }
      std::size_t i = Hash{}(s.key) & mask;
      while (!(slots_[i].key == kEmpty)) {
        i = (i + 1) & mask;
      }
      slots_[i] = s;
    }
  }

  std::vector<Slot> slots_;
  std::size_t size_{0};
};

} // namespace te
