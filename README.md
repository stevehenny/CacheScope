# CacheScope

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg?style=flat-square)](LICENSE)  
[![C++ Version](https://img.shields.io/badge/C%2B%2B-20-blue.svg?style=flat-square)](https://isocpp.org/)  
[![Issues](https://img.shields.io/github/issues/stevehenny/CacheScope?style=flat-square)](https://github.com/stevehenny/CacheScope/issues)  
[![Stars](https://img.shields.io/github/stars/stevehenny/CacheScope?style=flat-square)](https://github.com/stevehenny/CacheScope/stargazers)  

**CacheScope** is a visualization and analysis tool for cache line bouncing and false sharing in multithreaded C++ programs.  
It helps developers identify performance-critical areas where threads contend on the same cache lines, enabling optimization of data structures and memory access patterns.


##Dependencies

Install the following:

// on Arch (GLFW package may be glfw-x11 or glfw-wayland)
`sudo pacman -S cli11 libdwarf libelf libpfm glfw mesa`

// on Ubuntu/Debian
`sudo apt install libcli11-dev libdwarf-dev libelf-dev libpfm4-dev libglfw3-dev libgl1-mesa-dev`

// Fedora
`sudo dnf install cli11-devel libdwarf-devel elfutils-libelf-devel libpfm-devel glfw-devel mesa-libGL-devel`

ImGui is included as a git submodule under `third_party/imgui`.

---

## **Motivation**

False sharing is one of the most subtle and impactful performance killers in multithreaded applications. Despite its importance, no user-friendly tool exists that correlates:

- C++ struct layouts
- Thread memory access patterns
- CPU cache line mapping

CacheScope fills this gap, giving developers actionable insights to optimize cache usage.

---

## **Features Needed to Add**

- **Clang-based struct layout extraction**: Automatically analyze C++ struct and class memory layouts.  
- **Per-thread memory access tracing**: Instrument your code to log read/write access patterns.  
- **Cache line simulation**: Map memory addresses to CPU cache lines, detect conflicts, and highlight false sharing.  
- **Visualization-ready output**: Export JSON/CSV reports suitable for heatmaps, timelines, or custom visualizations.  
- **Multi-thread support**: Analyze programs using multiple threads and identify cross-thread conflicts.

---
## **Features Added**

- **Report output**: Export false sharing results to Markdown or JSON with
  `--report-md` and `--report-json`.

---

## **Reports**

```bash
./cache_scope analyze ./my_binary --report-md report.md --report-json report.json
```

---

## **Installation**

```bash
git clone https://github.com/yourusername/CacheScope.git
cd CacheScope
git submodule update --init --recursive
mkdir build && cd build
cmake ..
make
```

### GUI (ImGui)

```bash
./cache_scope_gui report.json
```

Disable the GUI build with `-DCACHESCOPE_BUILD_GUI=OFF` if you only want the CLI.

### Notes for stack-variable attribution

To get non-zero **Stack-attributed samples**, build the *target program you are analyzing* with debug info (`-g`) and avoid stripping symbols.

CacheScope uses DWARF CFI (`.eh_frame`/`.debug_frame`) to compute CFA at each sample, so frame pointers are not required.
