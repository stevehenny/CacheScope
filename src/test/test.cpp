

#include <cstdlib>
int main(int argc, char* argv[]) {
  int* ptr = (int*)malloc(1000);
  ptr[0]   = 2;
  return 0;
}
