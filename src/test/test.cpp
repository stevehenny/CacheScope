

#include <array>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <unordered_map>
#include <vector>

int main(int argc, char* argv[]) {
  int* ptr = (int*)malloc(1000);
  // int x{0};
  // int y{0};
  ptr       = (int*)realloc(ptr, 2000);
  ptr[0]    = 2;
  int* ptr2 = new int[5];

  std::atomic<int> at;
  std::array<int, 5> ar;
  std::deque<int> deq;

  // FIXME: STL types with allocaters and complex types are not currently
  //  supported. Need to fix this. Probably a problem with the recursion
  std::vector<bool> si;
  std::vector<int> vec;
  std::unordered_map<int, int> mapTest;
  int arr[5];
  ptr2[0] = 10;
  delete[] ptr2;

  free(ptr);
  return 0;
}
