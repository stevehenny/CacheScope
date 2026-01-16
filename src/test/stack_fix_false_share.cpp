
#include <atomic>
#include <new>
#include <thread>
#include <vector>

struct alignas(std::hardware_constructive_interference_size) PaddedCounter {
  std::atomic<int> value;
};

void thread_method(PaddedCounter counters[], int id) {
  for (int i = 0; i < 100'000'000; ++i) {
    counters[id].value.fetch_add(1, std::memory_order_relaxed);
  }
}

int main() {
  std::vector<std::thread> threads;

  PaddedCounter counters[4];
  for (int i = 0; i < 4; ++i) {
    threads.emplace_back(thread_method, counters, i);
  }
  for (auto& t : threads) {
    t.join();
  }
}
