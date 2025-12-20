#include <CLI/CLI.hpp>
#include <string>

#include "CLI/CLI.hpp"
#include "extractor.hpp"
int main(int argc, char* argv[]) {
  CLI::App app("CacheScope: Analyze and visualize CPU cache behavior",
               "cache_scope");

  app.require_subcommand(1);

  bool verbose = false;
  app.add_flag("-v,--verbose", verbose, "Enable verbose debugging output");

  std::string binary;
  CLI::App* analyze =
    app.add_subcommand("analyze", "Analyze cache behavior of a binary");

  analyze->add_option("binary", binary, "Binary to analyze")
    ->required()
    ->check(CLI::ExistingFile);

  analyze->callback([&]() {
    Extractor ext{binary};
    ext.create_registry();
  });

  std::string trace_file;
  CLI::App* visualize =
    app.add_subcommand("visualize", "Visualize cache trace output");

  visualize->add_option("-t,--trace", trace_file, "Trace file");
  visualize->callback([&]() {
    // TODO: Add visualize callback here
  });

  CLI11_PARSE(app, argc, argv);
  return 0;
}
