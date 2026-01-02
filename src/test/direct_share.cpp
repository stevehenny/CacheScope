#include <atomic>
#include <thread>
#include <vector>
std::atomic<int> counter;

void thread_method() {
  for (int i{}; i < 100000000; ++i) {
    counter.fetch_add(1);
  }
}

int main(int argc, char* argv[]) {
  std::vector<std::thread> threads;
  int arr[100000];
  for (int i{}; i < 100000; ++i) {
    arr[i]++;
  }
  for (int i{}; i < 4; ++i) {
    threads.emplace_back(std::thread{thread_method});
  }

  for (auto& t : threads) {
    t.join();
  }
  return 0;
}
