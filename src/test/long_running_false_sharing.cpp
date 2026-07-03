#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using Counters = std::vector<std::atomic<int>>;

void increment_counters(Counters& counters, int id) {
  while (true) {
    counters[id].fetch_add(1, std::memory_order_relaxed);
  }
}

int main() {
  constexpr int kThreadCount = 4;
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);

  Counters counters(kThreadCount);
  for (int i = 0; i < kThreadCount; ++i) {
    counters[i].store(0);
    threads.emplace_back(increment_counters, std::ref(counters), i);
  }

  while (true) {
    // std::this_thread::sleep_for(std::chrono::seconds(1));
  }
}
