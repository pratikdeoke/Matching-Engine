// Chunked object pool with a free list.
//
// Why this exists: the matching path allocates one Order per accepted order and
// frees it on fill/cancel. Doing that through the general allocator puts malloc
// on the hot path and scatters orders across the heap, which shows up as cache
// misses while walking a price level. The pool hands out objects from
// contiguous chunks and recycles freed slots, so steady-state order churn does
// zero allocation.
//
// Deliberately *chunked* rather than one flat vector: growth must never
// invalidate pointers to live objects, because the order book links to them
// intrusively and the order index stores their addresses.
#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace te {

template <typename T> class ObjectPool {
public:
  explicit ObjectPool(std::size_t chunk_size = 4096)
      : chunk_size_(chunk_size ? chunk_size : 1) {}

  ObjectPool(const ObjectPool &) = delete;
  ObjectPool &operator=(const ObjectPool &) = delete;
  ObjectPool(ObjectPool &&) = default;
  ObjectPool &operator=(ObjectPool &&) = default;

  ~ObjectPool() = default;

  // Reserve capacity up-front so a benchmark or a trading session does not pay
  // for chunk allocation mid-run.
  void reserve(std::size_t n) {
    while (capacity_ < n) {
      add_chunk();
    }
  }

  template <typename... Args> [[nodiscard]] T *acquire(Args &&...args) {
    Slot *slot = free_;
    if (slot == nullptr) [[unlikely]] {
      add_chunk();
      slot = free_;
    }
    free_ = slot->next_free;
    ++live_;
    return new (static_cast<void *>(slot->storage)) T(std::forward<Args>(args)...);
  }

  void release(T *obj) noexcept {
    if (obj == nullptr) {
      return;
    }
    obj->~T();
    auto *slot = reinterpret_cast<Slot *>(obj);
    slot->next_free = free_;
    free_ = slot;
    --live_;
  }

  [[nodiscard]] std::size_t live() const noexcept { return live_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
  // A slot is either a constructed T or a free-list link. Overlaying the link
  // on the object storage keeps the pool's memory overhead at zero per slot.
  union Slot {
    alignas(T) unsigned char storage[sizeof(T)];
    Slot *next_free;
  };

  void add_chunk() {
    auto chunk = std::make_unique<Slot[]>(chunk_size_);
    // Thread the new slots onto the front of the free list. Linking in reverse
    // means the first acquire() after growth returns the lowest address, so a
    // fresh pool hands out orders in ascending memory order.
    for (std::size_t i = chunk_size_; i-- > 0;) {
      chunk[i].next_free = free_;
      free_ = &chunk[i];
    }
    capacity_ += chunk_size_;
    chunks_.push_back(std::move(chunk));
  }

  std::size_t chunk_size_;
  std::vector<std::unique_ptr<Slot[]>> chunks_;
  Slot *free_{nullptr};
  std::size_t capacity_{0};
  std::size_t live_{0};
};

} // namespace te
