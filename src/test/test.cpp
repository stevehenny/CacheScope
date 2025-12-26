

#include <cstdlib>
int main(int argc, char* argv[]) {
  int* ptr = (int*)malloc(1000);
  // int x{0};
  // int y{0};
  ptr       = (int*)realloc(ptr, 2000);
  ptr[0]    = 2;
  int* ptr2 = new int[5];

  int arr[5];
  ptr2[0] = 10;
  delete[] ptr2;

  free(ptr);
  return 0;
}
