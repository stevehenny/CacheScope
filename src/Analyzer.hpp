#include <ostream>
#include <string>
#include <unordered_map>

using std::string, std::ostream, std::unordered_map;
class Analyzer {
  public:
  Analyzer();
  ~Analyzer();

  void analyze_bin(string bin);
  ostream get_report();

  private:
  ostream report;
};
