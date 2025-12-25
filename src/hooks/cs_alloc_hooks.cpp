#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <link.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
#include <new>
#include <string>

#include "runtime/AllocationTracker.hpp"

/* ============================================================
 * libc bootstrap symbols
 * ============================================================ */
extern "C" void* __libc_malloc(size_t);
extern "C" void __libc_free(void*);

/* ============================================================
 * Global state
 * ============================================================ */
static thread_local bool in_hook = false;
static std::string trace_path    = "trace.bin";
static std::mutex trace_mutex;

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
 * User binary range
 * ============================================================ */
static uintptr_t user_binary_start = 0;
static uintptr_t user_binary_end   = 0;

static int phdr_callback(struct dl_phdr_info* info, size_t, void*) {
  if (!info->dlpi_name || info->dlpi_name[0] == '\0') {
    user_binary_start = info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
      const auto& phdr = info->dlpi_phdr[i];
      if (phdr.p_type == PT_LOAD) {
        user_binary_end = std::max(
          user_binary_end, info->dlpi_addr + phdr.p_vaddr + phdr.p_memsz);
      }
    }
    return 1;
  }
  return 0;
}

static void init_user_binary_range() {
  static bool initialized = false;
  if (initialized) return;
  dl_iterate_phdr(phdr_callback, nullptr);
  initialized = true;
}

/* ============================================================
 * Lazy symbol resolution
 * ============================================================ */
static void resolve_real_funcs() {
  static bool resolving = false;
  if (real_malloc || resolving) return;
  resolving = true;

  real_malloc  = (void* (*)(size_t))dlsym(RTLD_NEXT, "malloc");
  real_free    = (void (*)(void*))dlsym(RTLD_NEXT, "free");
  real_calloc  = (void* (*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
  real_realloc = (void* (*)(void*, size_t))dlsym(RTLD_NEXT, "realloc");
  real_mmap =
    (void* (*)(void*, size_t, int, int, int, off_t))dlsym(RTLD_NEXT, "mmap");
  real_munmap = (int (*)(void*, size_t))dlsym(RTLD_NEXT, "munmap");

  resolving = false;
}

/* ============================================================
 * Capture first user-callsite in main binary
 * ============================================================ */
static inline uintptr_t capture_user_callsite() {
  init_user_binary_range();

  void* buffer[8];
  int n = backtrace(buffer, 8);
  for (int i = 2; i < n; ++i) {  // skip hook frames
    uintptr_t ip = reinterpret_cast<uintptr_t>(buffer[i]);
    if (ip >= user_binary_start && ip < user_binary_end) {
      return ip;
    }
  }
  return 0;  // no user frame found
}

/* ============================================================
 * Tracking helpers
 * ============================================================ */
static inline void track_alloc(void* ptr, size_t size, AllocationKind kind,
                               int fd = -1) {
  if (!ptr) return;

  uintptr_t ip = capture_user_callsite();
  if (ip == 0) return;  // skip internal allocations

  if (AllocationTracker::instance().is_enabled())
    AllocationTracker::instance().register_allocation(ptr, size, ip, kind, fd);

  std::lock_guard<std::mutex> guard(trace_mutex);
  std::ofstream out(trace_path, std::ios::binary | std::ios::app);
  if (out.is_open()) {
    Allocation alloc;
    alloc.base        = reinterpret_cast<uintptr_t>(ptr);
    alloc.size        = size;
    alloc.callsite_ip = ip;
    alloc.kind        = kind;
    alloc.mmap_fd     = fd;
    alloc.type        = nullptr;
    out.write(reinterpret_cast<const char*>(&alloc), sizeof(alloc));
  }
}

static inline void track_free(void* ptr) {
  if (!ptr || !AllocationTracker::instance().is_enabled()) return;
  AllocationTracker::instance().unregister_allocation(ptr);
}

/* ============================================================
 * malloc / free hooks
 * ============================================================ */
extern "C" void* malloc(size_t size) {
  resolve_real_funcs();
  if (in_hook) return real_malloc ? real_malloc(size) : __libc_malloc(size);

  in_hook   = true;
  void* ptr = real_malloc ? real_malloc(size) : __libc_malloc(size);
  track_alloc(ptr, size, AllocationKind::HEAP);
  in_hook = false;
  return ptr;
}

extern "C" void free(void* ptr) {
  if (!ptr) return;
  resolve_real_funcs();
  if (!in_hook) {
    in_hook = true;
    track_free(ptr);
    in_hook = false;
  }
  real_free ? real_free(ptr) : __libc_free(ptr);
}

extern "C" void* calloc(size_t n, size_t size) {
  resolve_real_funcs();
  if (in_hook) return __libc_malloc(n * size);

  in_hook   = true;
  void* ptr = real_calloc ? real_calloc(n, size) : __libc_malloc(n * size);
  if (ptr) std::memset(ptr, 0, n * size);
  track_alloc(ptr, n * size, AllocationKind::HEAP);
  in_hook = false;
  return ptr;
}

extern "C" void* realloc(void* old_ptr, size_t size) {
  resolve_real_funcs();
  if (in_hook)
    return real_realloc ? real_realloc(old_ptr, size) : __libc_malloc(size);

  in_hook = true;
  if (old_ptr) track_free(old_ptr);
  void* ptr = real_realloc ? real_realloc(old_ptr, size) : __libc_malloc(size);
  track_alloc(ptr, size, AllocationKind::HEAP);
  in_hook = false;
  return ptr;
}

extern "C" void* mmap(void* addr, size_t len, int prot, int flags, int fd,
                      off_t off) {
  resolve_real_funcs();
  if (in_hook) return real_mmap(addr, len, prot, flags, fd, off);

  in_hook   = true;
  void* ptr = real_mmap(addr, len, prot, flags, fd, off);
  if (ptr != MAP_FAILED) track_alloc(ptr, len, AllocationKind::MMAP, fd);
  in_hook = false;
  return ptr;
}

extern "C" int munmap(void* addr, size_t len) {
  resolve_real_funcs();
  if (!in_hook) {
    in_hook = true;
    track_free(addr);
    in_hook = false;
  }
  return real_munmap(addr, len);
}

/* ============================================================
 * C++ new / delete
 * ============================================================ */
void* operator new(size_t size) {
  void* ptr = malloc(size);
  if (!ptr) throw std::bad_alloc();
  return ptr;
}
void* operator new[](size_t size) {
  void* ptr = malloc(size);
  if (!ptr) throw std::bad_alloc();
  return ptr;
}
void operator delete(void* ptr) noexcept { free(ptr); }
void operator delete[](void* ptr) noexcept { free(ptr); }
void operator delete(void* ptr, size_t) noexcept { free(ptr); }
void operator delete[](void* ptr, size_t) noexcept { free(ptr); }

void* operator new(size_t size, std::align_val_t align) {
  void* ptr = aligned_alloc(static_cast<size_t>(align), size);
  if (!ptr) throw std::bad_alloc();
  return ptr;
}
void operator delete(void* ptr, std::align_val_t) noexcept { free(ptr); }
void operator delete[](void* ptr, std::align_val_t) noexcept { free(ptr); }
