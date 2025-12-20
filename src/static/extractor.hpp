#include <cstdio>
#include <string>

#include "registry.hpp"

using std::string;

class Extractor {
public:
  Extractor(const string& bin);
  ~Extractor();
  void create_registry();
  const StructRegistry& get_registry() const;

private:
  int bin_fd;
  StructRegistry registry;
};
