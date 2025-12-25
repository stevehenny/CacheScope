#include <fstream>
#include <string>
#include <vector>

#include "runtime/AllocationTracker.hpp"
class Parser {
public:
  Parser(std::string binary);
  ~Parser();

  void populate_allocs();
  std::vector<Allocation>& get_allocs();

private:
  std::ifstream _stream;
  std::vector<Allocation> _allocs;
};
