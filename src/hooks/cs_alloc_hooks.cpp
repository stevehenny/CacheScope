#define _GNU_SOURCE
#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <unordered_map>

#include "dwarf/Extractor.hpp"  // To get TypeInfo / StructInfo if needed

std::mutex alloc_mutex;

struct Allocation {
  void* ptr;
  size_t size;
  TypeInfo* type;  // optional
};
std::unordered_map<void*, Allocation> allocations;

// Function pointers to real libc functions
static void* (*real_malloc)(size_t)         = nullptr;
static void (*real_free)(void*)             = nullptr;
static void* (*real_calloc)(size_t, size_t) = nullptr;
static void* (*real_realloc)(void*, size_t) = nullptr;

extern "C" {

// malloc hook
void* malloc(size_t size) {
  static bool init = false;
  if (!init) {
    real_malloc  = (void* (*)(size_t))dlsym(RTLD_NEXT, "malloc");
    real_free    = (void (*)(void*))dlsym(RTLD_NEXT, "free");
    real_calloc  = (void* (*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
    real_realloc = (void* (*)(void*, size_t))dlsym(RTLD_NEXT, "realloc");
    init         = true;
  }

  void* ptr = real_malloc(size);

  {
    std::lock_guard<std::mutex> lock(alloc_mutex);
    allocations[ptr] = Allocation{ptr, size, nullptr};
    printf("[CacheScope] malloc %p size %zu\n", ptr, size);
  }
  return ptr;
}

// free hook
void free(void* ptr) {
  {
    std::lock_guard<std::mutex> lock(alloc_mutex);
    allocations.erase(ptr);
    printf("[CacheScope] free %p\n", ptr);
  }
  real_free(ptr);
}

// calloc hook
void* calloc(size_t nmemb, size_t size) {
  void* ptr = real_calloc(nmemb, size);
  {
    std::lock_guard<std::mutex> lock(alloc_mutex);
    allocations[ptr] = Allocation{ptr, nmemb * size, nullptr};
    printf("[CacheScope] calloc %p size %zu\n", ptr, nmemb * size);
  }
  return ptr;
}

// realloc hook
void* realloc(void* old_ptr, size_t size) {
  void* ptr = real_realloc(old_ptr, size);
  {
    std::lock_guard<std::mutex> lock(alloc_mutex);
    if (old_ptr) allocations.erase(old_ptr);
    allocations[ptr] = Allocation{ptr, size, nullptr};
    printf("[CacheScope] realloc %p size %zu\n", ptr, size);
  }
  return ptr;
}

}  // extern "C"
