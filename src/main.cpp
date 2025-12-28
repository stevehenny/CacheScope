#include <limits.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <CLI/CLI.hpp>
#include <iostream>
#include <string>

#include "dwarf/Extractor.hpp"
#include "runtime/Tracer.hpp"

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
      if (var.type) std::cout << var.type->name << '\n';
      std::cout << var.name << '\n';
      std::cout << var.function << ": " << var.frame_offset << '\n';
      std::cout << "stack frame offset: " << var.frame_offset << '\n';
      std::cout << '\n';
    }

    // ------------------------------------------------------------
    // Phase 2: Fork + PERF attach (CORRECTLY SYNCHRONIZED)
    // ------------------------------------------------------------
    TracerConfig cfg{};
    cfg.event          = CacheEvent::L1D_LOAD;
    cfg.sample_period  = 1000;
    cfg.precise_ip     = true;
    cfg.exclude_kernel = true;
    cfg.exclude_hv     = true;
    cfg.cpu            = TracerConfig::detect_cpu_vendor();

    pid_t child = fork();

    if (child == 0) {
      // Child: stop immediately so parent can attach perf
      raise(SIGSTOP);

      char* const args[] = {const_cast<char*>(binary.c_str()), nullptr};

      execvp(binary.c_str(), args);
      _exit(127);
    }

    // Parent
    int status = 0;
    waitpid(child, &status, WUNTRACED);

    if (!WIFSTOPPED(status)) {
      throw std::runtime_error("Child did not stop as expected");
    }

    // Attach perf AFTER child is stopped
    Tracer tracer{child, cfg};
    tracer.start();

    // Resume child
    kill(child, SIGCONT);

    // Wait for completion
    waitpid(child, &status, 0);
    tracer.stop();

    if (WIFEXITED(status)) {
      std::cout << "Target exited with code " << WEXITSTATUS(status) << "\n";
    }

    // ------------------------------------------------------------
    // Phase 3: (future) correlate samples → DWARF objects
    // ------------------------------------------------------------
    auto samples = tracer.drain();
    std::cout << "Collected samples: " << samples.size() << "\n";
    for (auto sample : samples) {
      std::cout << "CPU: " << sample.cpu << '\n';
      std::cout << "TID: " << std::dec << sample.tid << '\n';
      std::cout << "ADDR: 0x" << std::hex << sample.addr << '\n';
      std::cout << "IP: 0x" << std::hex << sample.ip << '\n';
      std::cout << '\n';
    }
  });

  auto* visualize =
    app.add_subcommand("visualize", "Visualize cache trace output");

  std::string trace_file;
  visualize->add_option("-t,--trace", trace_file, "Trace file");

  CLI11_PARSE(app, argc, argv);
  return 0;
}
