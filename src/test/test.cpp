

#include <array>
#include <atomic>
#include <cstdlib>
#include <deque>
#include <unordered_map>
#include <vector>

template <typename T>
class TemplateType {
public:
  std::vector<T>& get_vec() { return typeVec; }

private:
  std::vector<T> typeVec;
};

int main(int argc, char* argv[]) {
  int* ptr = (int*)malloc(1000);
  int x{0};
  int y{0};
  ptr       = (int*)realloc(ptr, 2000);
  ptr[0]    = 2;
  int* ptr2 = new int[5];

  x = 2;
  // std::atomic<int> at;
  // std::array<int, 5> ar;
  // std::deque<int> deq;
  //
  // std::vector<bool> si;
  // std::vector<int> vec;
  // std::unordered_map<int, int> mapTest;
  //
  // TemplateType<bool> boolVec;
  // int arr[5];
  ptr2[0] = 10;
  delete[] ptr2;

  free(ptr);
  return 0;
}
