#pragma once

#include <filesystem>
#include <istream>
#include <ostream>

#include "core/Error.hpp"
#include "core/Result.hpp"
#include "report/Report.hpp"

struct JsonReport {
  static void write(std::ostream& output, const AnalysisResult& result);
  static cachescope::Result<AnalysisResult, cachescope::Error> read(
    std::istream& input);
  static cachescope::Result<AnalysisResult, cachescope::Error> read_file(
    const std::filesystem::path& path);
  static cachescope::Result<void, cachescope::Error> write_atomic(
    const std::filesystem::path& path, const AnalysisResult& result);
};
