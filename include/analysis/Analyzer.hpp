#include <any>
#include <ostream>
#include <string>
#include <unordered_map>

using std::string, std::ostream, std::unordered_map, std::any;

struct AnalyzerStruct {
  string structName;
  any structType;
};

class Analyzer {
  public:
  Analyzer();
  ~Analyzer();

  void analyze_bin(string bin);
  ostream get_report();

  private:
  ostream report;
  unordered_map<string, AnalyzerStruct> structMap;
};
