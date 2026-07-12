#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

struct Counters {
  std::atomic<int> a;
  std::atomic<int> b;
  std::atomic<int> c;
  std::atomic<int> d;
};

Counters counters;

void thread_method(int id) {
  for (int i = 0; i < 100'000'000; ++i) {
    if (id == 0) counters.a.fetch_add(1, std::memory_order_relaxed);
    if (id == 1) counters.b.fetch_add(1, std::memory_order_relaxed);
    if (id == 2) counters.c.fetch_add(1, std::memory_order_relaxed);
    if (id == 3) counters.d.fetch_add(1, std::memory_order_relaxed);
  }
}

int main() {
  std::vector<std::thread> threads;
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back(thread_method, i);
  }
  for (auto& t : threads) {
    t.join();
  }
}
