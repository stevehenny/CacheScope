#include <thread>
struct Counter {
  int count;
  Counter() : count(0) {}
  void increment() { count++; }
};

Counter counters[4];

int main(int argc, char* argv[]) {
  for (int i = 0; i < 4; ++i) {
    std::thread([i]() {
      for (int j = 0; j < 100'000'000; ++j) {
        counters[i].increment();
      }
    }).join();
  }
}
