#pragma once
#include <atomic>
#include <cstdint>

#include "common/Constants.hpp"
#include "common/Types.hpp"

static constexpr size_t MAX_ALLOCS   = 1 << 20;  // ~1M entries
static constexpr uintptr_t EMPTY     = 0;
static constexpr uintptr_t TOMBSTONE = 1;

struct Allocation {
  uintptr_t base;
  size_t size;
  uintptr_t callsite_ip;
  TypeInfo* type;
  AllocationKind kind;
  int mmap_fd{-1};  // -1 for anonymous / heap
};

struct Entry {
  std::atomic<uintptr_t> base{EMPTY};
  std::atomic<size_t> size{0};
  std::atomic<uintptr_t> callsite_ip{0};
  std::atomic<AllocationKind> kind{AllocationKind::HEAP};
  std::atomic<int> mmap_fd{-1};
};

class AllocationTracker {
public:
  static AllocationTracker& instance() {
    static AllocationTracker t;
    return t;
  }

  /* ---------------- Enable / Disable Tracking ---------------- */
  void enable() { _enabled.store(true, std::memory_order_release); }
  void disable() { _enabled.store(false, std::memory_order_release); }
  bool is_enabled() const { return _enabled.load(std::memory_order_acquire); }

  /* ---------------- Insert ---------------- */
  void register_allocation(void* ptr, size_t size, uintptr_t callsite_ip,
                           AllocationKind kind, int fd = -1) {
    if (!ptr || size == 0 || !_enabled.load(std::memory_order_relaxed)) return;

    uintptr_t base = reinterpret_cast<uintptr_t>(ptr);
    size_t idx     = hash(base);

    for (size_t i = 0; i < MAX_ALLOCS; ++i) {
      Entry& e           = _table[idx];
      uintptr_t expected = EMPTY;

      if (e.base.compare_exchange_weak(expected, base,
                                       std::memory_order_acq_rel)) {
        e.size.store(size, std::memory_order_relaxed);
        e.callsite_ip.store(callsite_ip, std::memory_order_relaxed);
        e.kind.store(kind, std::memory_order_relaxed);
        e.mmap_fd.store(fd, std::memory_order_relaxed);
        return;
      }

      idx = (idx + 1) & (MAX_ALLOCS - 1);
    }
  }

  /* ---------------- Remove ---------------- */
  void unregister_allocation(void* ptr) {
    if (!ptr || !_enabled.load(std::memory_order_relaxed)) return;

    uintptr_t base = reinterpret_cast<uintptr_t>(ptr);
    size_t idx     = hash(base);

    for (size_t i = 0; i < MAX_ALLOCS; ++i) {
      Entry& e      = _table[idx];
      uintptr_t cur = e.base.load(std::memory_order_acquire);

      if (cur == base) {
        e.base.store(TOMBSTONE, std::memory_order_release);
        return;
      }
      if (cur == EMPTY) return;

      idx = (idx + 1) & (MAX_ALLOCS - 1);
    }
  }

  /* ---------------- Lookup ---------------- */
  const Allocation* find(uintptr_t addr) const {
    if (!_enabled.load(std::memory_order_relaxed)) return nullptr;

    size_t idx = hash(addr);

    for (size_t i = 0; i < MAX_ALLOCS; ++i) {
      const Entry& e = _table[idx];
      uintptr_t base = e.base.load(std::memory_order_acquire);

      if (base == EMPTY) return nullptr;
      if (base > TOMBSTONE) {
        size_t size = e.size.load(std::memory_order_relaxed);
        if (addr >= base && addr < base + size) {
          _scratch.base        = base;
          _scratch.size        = size;
          _scratch.callsite_ip = e.callsite_ip.load(std::memory_order_relaxed);
          _scratch.kind        = e.kind.load(std::memory_order_relaxed);
          _scratch.mmap_fd     = e.mmap_fd.load(std::memory_order_relaxed);
          return &_scratch;
        }
      }
      idx = (idx + 1) & (MAX_ALLOCS - 1);
    }
    return nullptr;
  }

  Entry* get_table() { return _table; }

private:
  AllocationTracker() = default;

  static size_t hash(uintptr_t x) { return (x >> 4) & (MAX_ALLOCS - 1); }

  alignas(64) Entry _table[MAX_ALLOCS];
  mutable Allocation _scratch;

  std::atomic<bool> _enabled{false};
};
