#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <sched.h>

namespace {

constexpr size_t kStride = 4096;
constexpr size_t kLines  = 9;
alignas(kStride) std::array<uint8_t, kLines * kStride> data{};

}  // namespace

int main() {
  const int cpu = sched_getcpu();
  if (cpu >= 0) {
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    CPU_SET(cpu, &affinity);
    (void)sched_setaffinity(0, sizeof(affinity), &affinity);
  }

  volatile uint64_t sum = 0;
  for (size_t repetition = 0; repetition < 20'000'000; ++repetition) {
    for (size_t line = 0; line < kLines; ++line) {
      sum = sum + data[line * kStride];
    }
  }

  std::cout << sum << '\n';
  return 0;
}
