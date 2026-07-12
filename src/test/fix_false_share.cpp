
#include <atomic>
#include <new>
#include <thread>
#include <vector>

struct alignas(std::hardware_constructive_interference_size) PaddedCounter {
  std::atomic<int> value;
};

PaddedCounter counters[4];
void thread_method(int id) {
  for (int i = 0; i < 100'000'000; ++i) {
    counters[id].value.fetch_add(1, std::memory_order_relaxed);
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
