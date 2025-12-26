#include <limits.h>
#include <sys/wait.h>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <iostream>
#include <string>

#include "dwarf/Extractor.hpp"
#include "runtime/AllocationTracker.hpp"
#include "runtime/Parser.hpp"

static std::string get_self_dir() {
  char buf[PATH_MAX];
  ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len <= 0) return "";

  buf[len] = '\0';
  std::string path(buf);
  return path.substr(0, path.find_last_of('/'));
}

int main(int argc, char* argv[]) {
  CLI::App app("CacheScope: Analyze and visualize CPU cache behavior",
               "cache_scope");

  app.require_subcommand(1);

  bool verbose = false;
  app.add_flag("-v,--verbose", verbose, "Enable verbose debugging output");

  std::string binary;
  auto* analyze =
    app.add_subcommand("analyze", "Analyze cache behavior of a binary");

  analyze->add_option("binary", binary)->required()->check(CLI::ExistingFile);

  analyze->callback([&]() {
    // ------------------------------------------------------------
    // Phase 1: DWARF extraction (parent process)
    // ------------------------------------------------------------
    Extractor ext{binary};
    ext.create_registry();

    if (verbose) {
      for (const auto& [k, v] : ext.get_registry().get_map()) {
        std::cout << k << ": " << v.size << '\n';
      }
    }

    const auto& ref = ext.get_stack_objects();
    std::cout << ref.size() << '\n';
    for (const auto& var : ref) {
      // std::cout << var.function << '\n';
      if (var.type) std::cout << var.type->name << '\n';
      std::cout << var.name << '\n';
      std::cout << var.function << ": " << var.frame_offset << '\n';
      std::cout << "0x" << std::hex << var.high_pc << "->" << "0x" << std::hex
                << var.low_pc << '\n';
    }

    // ------------------------------------------------------------
    // Phase 2: Run instrumented binary
    // ------------------------------------------------------------
    pid_t pid = fork();
    if (pid == 0) {
      std::string self_dir = get_self_dir();

      // Example layout:
      //   build/cache_scope
      //   build/lib/libcachescope_alloc.so
      std::string preload = self_dir + "/hooks/libcachescope_alloc.so";

      setenv("LD_PRELOAD", preload.c_str(), 1);
      setenv("CACHESCOPE_ENABLE", "1", 1);
      setenv("CACHESCOPE_TRACE", "trace.bin", 1);

      AllocationTracker::instance().enable();
      execl(binary.c_str(), binary.c_str(), nullptr);
      AllocationTracker::instance().disable();
      _exit(127);
    }
    // PARENT
    int status = 0;
    waitpid(pid, &status, 0);

    Parser parser{"trace.bin"};
    // std::vector<Allocation>& allocs = parser.get_allocs();
    // for (auto alloc : allocs) {
    //   std::cout << "base=0x" << std::hex << alloc.base << " size=" <<
    //   std::dec
    //             << alloc.size << " kind=" << (int)alloc.kind << "callsite =
    //             0x"
    //             << std::hex << alloc.callsite_ip << "\n";
    // }
    // for(auto entry : AllocationTracker::instance().get_table())
    if (WIFEXITED(status)) {
      std::cout << "Target exited with code " << WEXITSTATUS(status) << "\n";

      if (std::filesystem::exists("trace.bin")) {
        std::filesystem::remove("trace.bin");
      }
    }
  });

  auto* visualize =
    app.add_subcommand("visualize", "Visualize cache trace output");

  std::string trace_file;
  visualize->add_option("-t,--trace", trace_file, "Trace file");

  CLI11_PARSE(app, argc, argv);
  return 0;
}
