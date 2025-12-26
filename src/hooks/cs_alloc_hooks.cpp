#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <new>

#include "runtime/AllocationTracker.hpp"

/* ============================================================
 * Minimal allocation event format
 * ============================================================ */

/* ============================================================
 * Globals (NO STL)
 * ============================================================ */

static thread_local bool in_hook = false;
static std::atomic<bool> tracking_enabled{false};

static int trace_fd = -1;

/* ============================================================
 * libc bootstrap symbols (ALWAYS SAFE)
 * ============================================================ */

extern "C" void* __libc_malloc(size_t);
extern "C" void __libc_free(void*);

/* ============================================================
 * Real libc function pointers
 * ============================================================ */

static void* (*real_malloc)(size_t)                            = nullptr;
static void (*real_free)(void*)                                = nullptr;
static void* (*real_calloc)(size_t, size_t)                    = nullptr;
static void* (*real_realloc)(void*, size_t)                    = nullptr;
static void* (*real_mmap)(void*, size_t, int, int, int, off_t) = nullptr;
static int (*real_munmap)(void*, size_t)                       = nullptr;

/* ============================================================
 * Utility
 * ============================================================ */

static inline uint64_t get_ip() {
  return (uint64_t)__builtin_return_address(0);
}

static inline void write_event(const Allocation& ev) {
  if (trace_fd < 0) return;
  ssize_t r = write(trace_fd, &ev, sizeof(ev));
  (void)r;
}

/* ============================================================
 * Initialization
 * ============================================================ */

static void resolve_symbols() {
  real_malloc  = (void* (*)(size_t))dlsym(RTLD_NEXT, "malloc");
  real_free    = (void (*)(void*))dlsym(RTLD_NEXT, "free");
  real_calloc  = (void* (*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
  real_realloc = (void* (*)(void*, size_t))dlsym(RTLD_NEXT, "realloc");
  real_mmap =
    (void* (*)(void*, size_t, int, int, int, off_t))dlsym(RTLD_NEXT, "mmap");
  real_munmap = (int (*)(void*, size_t))dlsym(RTLD_NEXT, "munmap");
}

__attribute__((constructor)) static void cachescope_init() {
  resolve_symbols();

  const char* enable = getenv("CACHESCOPE_ENABLE");
  if (!enable) return;

  const char* path = getenv("CACHESCOPE_TRACE");
  if (!path) return;

  trace_fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
  if (trace_fd < 0) return;

  tracking_enabled.store(true, std::memory_order_release);
}

__attribute__((destructor)) static void cachescope_fini() {
  if (trace_fd >= 0) {
    close(trace_fd);
    trace_fd = -1;
  }
}

/* ============================================================
 * malloc family
 * ============================================================ */

extern "C" void* malloc(size_t size) {
  if (in_hook) return real_malloc ? real_malloc(size) : __libc_malloc(size);

  if (!real_malloc) return __libc_malloc(size);

  in_hook   = true;
  void* ptr = real_malloc(size);

  if (ptr && tracking_enabled.load(std::memory_order_relaxed)) {
    Allocation ev{
      .base        = (uint64_t)ptr,
      .size        = size,
      .callsite_ip = get_ip(),
      .pid         = (uint32_t)getpid(),
      .type        = nullptr,  // don't know type from allocation
      .kind        = CScope::AllocationKind::HEAP,
      .is_free     = 0,
    };
    write_event(ev);
  }

  in_hook = false;
  return ptr;
}

extern "C" void free(void* ptr) {
  if (!ptr) return;

  if (!real_free) {
    __libc_free(ptr);
    return;
  }

  if (!in_hook && tracking_enabled.load(std::memory_order_relaxed)) {
    in_hook = true;
    Allocation ev{
      .base        = (uint64_t)ptr,
      .size        = 0,
      .callsite_ip = get_ip(),
      .pid         = (uint32_t)getpid(),
      .type        = nullptr,
      .kind        = CScope::AllocationKind::HEAP,
      .is_free     = 1,
    };
    write_event(ev);
    in_hook = false;
  }

  real_free(ptr);
}

extern "C" void* calloc(size_t n, size_t size) {
  size_t total = n * size;

  if (in_hook) return real_calloc ? real_calloc(n, size) : __libc_malloc(total);

  if (!real_calloc) return __libc_malloc(total);

  in_hook   = true;
  void* ptr = real_calloc(n, size);

  if (ptr && tracking_enabled.load(std::memory_order_relaxed)) {
    Allocation ev{
      .base        = (uint64_t)ptr,
      .size        = total,
      .callsite_ip = get_ip(),
      .pid         = (uint32_t)getpid(),
      .type        = nullptr,
      .kind        = CScope::AllocationKind::HEAP,
      .is_free     = 0,
    };
    write_event(ev);
  }

  in_hook = false;
  return ptr;
}

extern "C" void* realloc(void* old_ptr, size_t size) {
  if (in_hook)
    return real_realloc ? real_realloc(old_ptr, size) : __libc_malloc(size);

  if (!real_realloc) return __libc_malloc(size);

  in_hook = true;

  if (old_ptr && tracking_enabled.load(std::memory_order_relaxed)) {
    Allocation ev{
      .base        = (uint64_t)old_ptr,
      .size        = 0,
      .callsite_ip = get_ip(),
      .pid         = (uint32_t)getpid(),
      .type        = nullptr,
      .kind        = CScope::AllocationKind::HEAP,
      .is_free     = 1,
    };
    write_event(ev);
  }

  void* ptr = real_realloc(old_ptr, size);

  if (ptr && tracking_enabled.load(std::memory_order_relaxed)) {
    Allocation ev{
      .base        = (uint64_t)ptr,
      .size        = size,
      .callsite_ip = get_ip(),
      .pid         = (uint32_t)getpid(),
      .type        = nullptr,
      .kind        = CScope::AllocationKind::HEAP,
      .is_free     = 0,
    };
    write_event(ev);
  }

  in_hook = false;
  return ptr;
}

/* ============================================================
 * mmap family
 * ============================================================ */

extern "C" void* mmap(void* addr, size_t len, int prot, int flags, int fd,
                      off_t off) {
  if (!real_mmap) return MAP_FAILED;

  if (in_hook) return real_mmap(addr, len, prot, flags, fd, off);

  in_hook   = true;
  void* ptr = real_mmap(addr, len, prot, flags, fd, off);

  if (ptr != MAP_FAILED && tracking_enabled.load(std::memory_order_relaxed)) {
    Allocation ev{
      .base        = (uint64_t)ptr,
      .size        = len,
      .callsite_ip = get_ip(),
      .pid         = (uint32_t)getpid(),
      .type        = nullptr,
      .kind        = CScope::AllocationKind::MMAP,
      .is_free     = 0,
    };
    write_event(ev);
  }

  in_hook = false;
  return ptr;
}

extern "C" int munmap(void* addr, size_t len) {
  if (!real_munmap) return -1;

  if (!in_hook && tracking_enabled.load(std::memory_order_relaxed)) {
    in_hook = true;
    Allocation ev{
      .base        = (uint64_t)addr,
      .size        = 0,
      .callsite_ip = get_ip(),
      .pid         = (uint32_t)getpid(),
      .type        = nullptr,
      .kind        = CScope::AllocationKind::MMAP,
      .is_free     = 1,
    };
    write_event(ev);
    in_hook = false;
  }

  return real_munmap(addr, len);
}

/* ============================================================
 * C++ new / delete
 * ============================================================ */

void* operator new(size_t size) {
  void* p = malloc(size);
  if (!p) throw std::bad_alloc();
  return p;
}

void operator delete(void* ptr) noexcept { free(ptr); }

void* operator new[](size_t size) {
  void* p = malloc(size);
  if (!p) throw std::bad_alloc();
  return p;
}

void operator delete[](void* ptr) noexcept { free(ptr); }

// sized delete (C++14+)
void operator delete(void* ptr, size_t) noexcept { free(ptr); }
void operator delete[](void* ptr, size_t) noexcept { free(ptr); }

// aligned new/delete (C++17)
void* operator new(size_t size, std::align_val_t align) {
  void* p = aligned_alloc((size_t)align, size);
  if (!p) throw std::bad_alloc();
  return p;
}

void operator delete(void* ptr, std::align_val_t) noexcept { free(ptr); }
