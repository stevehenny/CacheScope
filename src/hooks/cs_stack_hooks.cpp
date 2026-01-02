#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <iostream>

#include "common/Types.hpp"

/* ============================================================
 * Lazy globals
 * ============================================================ */
static inline std::atomic<bool>& tracking_enabled() {
  static std::atomic<bool> enabled{false};
  return enabled;
}

static inline bool& in_hook() {
  static thread_local bool value = false;
  return value;
}

static inline int& trace_fd() {
  static int fd = -1;
  return fd;
}

/* ============================================================
 * Utilities
 * ============================================================ */
static inline uint64_t get_ip() {
  return (uint64_t)__builtin_return_address(0);
}

static inline void* get_fp() { return __builtin_frame_address(0); }

static inline void write_stack_event(const RuntimeStackObject& obj) {
  if (trace_fd() < 0) return;
  write(trace_fd(), &obj, sizeof(obj));
}

/* ============================================================
 * Initialization
 * ============================================================ */
__attribute__((constructor)) static void stack_tracker_init() {
  const char* enable = getenv("CACHESCOPE_ENABLE");
  if (!enable) return;

  const char* path = getenv("CACHESCOPE_STACK_TRACE");
  if (!path) return;

  trace_fd() = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
  if (trace_fd() >= 0) tracking_enabled().store(true);
  std::cout << "[CacheScope] Stack hook initialized\n";
}

__attribute__((destructor)) static void stack_tracker_fini() {
  if (trace_fd() >= 0) {
    close(trace_fd());
    trace_fd() = -1;
  }
}

/* ============================================================
 * Hook functions
 * ============================================================ */
extern "C" void __attribute__((no_instrument_function))
__cyg_profile_func_enter(void* func, void* caller) {
  if (!tracking_enabled().load(std::memory_order_relaxed) || in_hook()) return;

  in_hook() = true;
  RuntimeStackObject obj{
    .function_ip = (uint64_t)func,
    .cfa         = (uint64_t)get_fp(),
    .callsite    = (uint64_t)caller,
    .pid         = (uint32_t)getpid(),
  };
  write_stack_event(obj);
  in_hook() = false;
}

extern "C" void __attribute__((no_instrument_function)) __cyg_profile_func_exit(
  void* func, void* caller) {
  (void)func;
  (void)caller;
  // Optional: log exits if needed
}
