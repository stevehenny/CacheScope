<p align="center">
  <img src="assets/CacheScope.png" alt="CacheScope logo" width="800">
</p>

<h1 align="center">CacheScope</h1>

<p align="center">
  <a href="LICENSE">
    <img src="https://img.shields.io/badge/License-MIT-blue.svg?style=flat-square" alt="MIT License">
  </a>
  <a href="https://isocpp.org/">
    <img src="https://img.shields.io/badge/C%2B%2B-20-blue.svg?style=flat-square" alt="C++20">
  </a>
  <a href="https://github.com/stevehenny/CacheScope/issues">
    <img src="https://img.shields.io/github/issues/stevehenny/CacheScope?style=flat-square" alt="GitHub issues">
  </a>
  <a href="https://github.com/stevehenny/CacheScope/stargazers">
    <img src="https://img.shields.io/github/stars/stevehenny/CacheScope?style=flat-square" alt="GitHub stars">
  </a>
</p>

**CacheScope** is a visualization and analysis tool for cache-line bouncing and false sharing in multithreaded C++ programs.

It helps developers identify performance-critical areas where threads contend on the same cache lines, enabling optimization of data structures and memory-access patterns.

---

## Dependencies

Install the required packages for your distribution.

### Arch Linux

The GLFW package may be `glfw-x11` or `glfw-wayland`, depending on your environment.

```bash
sudo pacman -S cli11 libdwarf libelf libpfm glfw mesa
```

### Ubuntu or Debian

```bash
sudo apt install libcli11-dev libdwarf-dev libelf-dev libpfm4-dev libglfw3-dev libgl1-mesa-dev
```

### Fedora

```bash
sudo dnf install cli11-devel libdwarf-devel elfutils-libelf-devel libpfm-devel glfw-devel mesa-libGL-devel
```

ImGui is included as a Git submodule under `third_party/imgui`.

---

## Motivation

False sharing is one of the most subtle and impactful performance problems in multithreaded applications. Despite its importance, few user-friendly tools correlate:

* C++ struct layouts
* Per-thread memory-access patterns
* CPU cache-line mappings

CacheScope fills this gap by giving developers actionable insights into cache usage and cross-thread contention.

---

## Features Added

* **False-sharing analysis:** Identify cache lines accessed by multiple threads.
* **Cache-line reporting:** Group sampled memory accesses by cache line.
* **Thread-aware statistics:** Track the threads involved in cache-line contention.
* **Bounce-score calculation:** Estimate the severity of cache-line bouncing.
* **Markdown reports:** Export human-readable reports with `--report-md`.
* **JSON reports:** Export machine-readable reports with `--report-json`.
* **Process monitoring:** Attach to a running process using monitor mode.
* **Graphical report viewer:** Inspect generated reports through the ImGui-based interface.

---

## Planned Features

* **Clang-based struct-layout extraction:** Automatically analyze C++ struct and class memory layouts.
* **Improved per-thread memory tracing:** Record detailed read and write access patterns.
* **Cache-line simulation:** Model cache-line ownership changes and highlight false sharing.
* **Additional visualization formats:** Produce heatmaps, timelines, and other interactive views.
* **Expanded multithreaded analysis:** Improve detection of complex cross-thread access patterns.

---

## Installation

Clone the repository and initialize its Git submodules:

```bash
git clone https://github.com/stevehenny/CacheScope.git
cd CacheScope
git submodule update --init --recursive
```

Configure and build the project:

```bash
mkdir build
cd build
cmake ..
cmake --build .
```

To disable the graphical interface and build only the command-line tool:

```bash
cmake .. -DCACHESCOPE_BUILD_GUI=OFF
cmake --build .
```

---

## Usage

### Analyze a program

```bash
./cache_scope analyze ./my_binary
```

Generate Markdown and JSON reports:

```bash
./cache_scope analyze ./my_binary \
  --report-md report.md \
  --report-json report.json
```

### Monitor a running process

```bash
./cache_scope monitor <pid>
```

Generate reports while monitoring:

```bash
./cache_scope monitor <pid> \
  --report-md report.md \
  --report-json report.json
```

Monitor mode attaches to a running process and skips DWARF-dependent attribution. It currently focuses on live sampling and cache-line reporting.

---

## GUI Report Viewer

Open a generated JSON report using either supported report-viewer command:

```bash
./cache_scope report report.json
```

```bash
./cache_scope_gui report.json
```

The GUI provides a visual overview of cache-line contention, participating threads, sample counts, and bounce scores.

---

## Stack-Variable Attribution

To obtain non-zero **stack-attributed samples**, build the target program with debugging information and avoid stripping its symbols.

For GCC or Clang:

```bash
-g
```

CacheScope uses DWARF call-frame information from `.eh_frame` or `.debug_frame` to calculate the canonical frame address. Frame pointers are therefore not required.

---

## License

CacheScope is released under the [MIT License](LICENSE).
