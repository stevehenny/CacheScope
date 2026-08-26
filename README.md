<p align="center">
  <img src="assets/CacheScope.png" alt="CacheScope logo" width="800">
</p>

<h1 align="center">CacheScope</h1>

CacheScope records Linux hardware-PMU memory samples and analyzes them for
suspected false sharing and cache-set thrashing. Captures use a reproducible
binary trace so collection and analysis can run separately.

## Production-beta scope

The beta targets Linux x86-64, kernel 5.8 or newer, Ubuntu 22.04/24.04, and
Intel PEBS or AMD IBS user-process profiling. The default scope is one target
process and its threads. Descendant processes, ARM64, kernel profiling,
containers, remote capture, and distributed analysis are deferred.

Findings are evidence-based suspected causes, not definitive diagnoses. Reports
include confidence, attribution evidence, capture capabilities, sample loss,
and limitations.

## Dependencies

Ubuntu and Debian:

~~~bash
sudo apt install \
  cmake g++ libcli11-dev libdwarf-dev libelf-dev libpfm4-dev \
  nlohmann-json3-dev libglfw3-dev libgl1-mesa-dev
~~~

Fedora:

~~~bash
sudo dnf install \
  cmake gcc-c++ cli11-devel libdwarf-devel elfutils-libelf-devel \
  libpfm-devel json-devel glfw-devel mesa-libGL-devel
~~~

Arch Linux:

~~~bash
sudo pacman -S cmake gcc cli11 libdwarf libelf libpfm nlohmann-json glfw mesa
~~~

ImGui is included as a Git submodule. If nlohmann_json is not installed, CMake
fetches pinned version 3.11.3.

## Build

~~~bash
git clone https://github.com/stevehenny/CacheScope.git
cd CacheScope
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
~~~

For a CLI-only build:

~~~bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCACHESCOPE_BUILD_GUI=OFF
~~~

Build options include CACHESCOPE_BUILD_GUI, CACHESCOPE_BUILD_TESTS,
CACHESCOPE_BUILD_WORKLOADS, CACHESCOPE_WARNINGS_AS_ERRORS, and
CACHESCOPE_ENABLE_PACKAGING. CacheScope does not override CMAKE_BUILD_TYPE.

## Preflight and permissions

Run diagnostics before collection:

~~~bash
build/src/cache_scope doctor
~~~

Doctor checks the architecture, CPU vendor, Intel/AMD PMU availability,
perf_event_paranoid, CAP_PERFMON, and cache topology. Event-dependent
capabilities are validated again when collection opens the PMU.

Prefer running unprivileged with the minimum perf permission required by local
policy. An administrator can grant CAP_PERFMON or adjust
kernel.perf_event_paranoid. CacheScope refuses to launch a target with effective
UID 0 unless --allow-root-target is explicit. Privileged attach remains
available with diagnostics.

## Commands

Record a target and its arguments:

~~~bash
build/src/cache_scope record -o recording.cst -- ./program arg1 arg2
~~~

Select events and period:

~~~bash
build/src/cache_scope record \
  -o recording.cst \
  -e mem-loads,mem-stores \
  -c 10000 \
  -- ./program arg1
~~~

AMD systems normally use ibs_op; doctor reports whether the IBS PMU exists.

Analyze offline:

~~~bash
build/src/cache_scope analyze \
  --trace recording.cst \
  --report-md report.md \
  --report-json report.json
~~~

Record and analyze in one command:

~~~bash
build/src/cache_scope run --keep-trace -- ./program arg1 arg2
~~~

Run uses a secure temporary trace. It deletes the trace only after successful
analysis and report output. On analysis/report failure it preserves and prints
the trace path. After success, run propagates the target exit status.

The deprecated analyze-binary form remains for compatibility, but automation
should use record, analyze --trace, or run.

Attach and optionally preserve a trace:

~~~bash
build/src/cache_scope monitor 1234
build/src/cache_scope monitor 1234 --output monitoring.cst
~~~

Open a report:

~~~bash
build/src/cache_scope report report.json
~~~

The GUI and CLI use the same canonical parser. The viewer accepts unversioned
legacy and schema-1.0 reports and shows capabilities, loss/truncation warnings,
confidence, suspected causes, cache topology, and attribution evidence.

## Traces and sensitive data

CST traces are little-endian, versioned, framed binary files with sequenced
records and CRC32 payload checksums. Metadata is capped at 4 MiB and each frame
at 1 MiB. Readers reject invalid magic, unsupported major versions, bad sizes,
broken sequences, checksum failures, truncation, and incomplete captures.
Newer minor versions and unknown future frame types are accepted safely.

Trace and report files use mode 0600. They can contain executable paths,
arguments, thread identifiers, and memory addresses. Treat them as sensitive
diagnostic artifacts.

## Reports and confidence

New JSON reports use schema 1.0. CacheScope reads unversioned reports through a
legacy adapter and writes only schema 1.0. JSON and Markdown replacement is
atomic.

Reports include tool/capture/platform metadata, PMU capabilities, lost and
malformed records, effective thresholds, topology, address-basis limitations,
suspected causes, confidence, attribution evidence, warnings, and remediation.
Unavailable capabilities are limitations, never zero-result conclusions.

## Exit statuses

- 0: tool and target success.
- 2: usage.
- 3: unsupported platform/capability.
- 4: permission or safe-operation refusal.
- 5: I/O or schema failure.
- 6: collection failure.
- 70: internal failure.
- 130: interrupted operation.

A successful run returns a nonzero target status unchanged; target signals
return 128 plus the signal number.

## Testing and release qualification

CTest covers detector controls, topology, trace framing/checksums, bounded
sample eviction, ring wrapping/malformed records, and legacy/schema-1.0 JSON.

CI builds GCC/Clang, Debug/Release, GUI on/off with warnings as errors. Separate
ASan/UBSan jobs run deterministic tests, and Ubuntu 22.04 receives a Release
smoke build.

Release tags additionally require manual Intel PEBS and AMD IBS qualification
using false-sharing, true-sharing, fixed-sharing, thrashing, streaming, and
non-thrashing controls compared with perf c2c and perf mem.

## License

CacheScope is released under the [MIT License](LICENSE).
