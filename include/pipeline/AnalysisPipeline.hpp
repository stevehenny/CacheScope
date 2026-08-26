#pragma once

#include <stop_token>
#include <string_view>

#include "core/Error.hpp"
#include "core/Models.hpp"
#include "core/Result.hpp"
#include "report/Report.hpp"

namespace cachescope {

class ProgressSink {
public:
  virtual ~ProgressSink() = default;
  virtual void update(std::string_view phase, double fraction) = 0;
};

class NullProgressSink final : public ProgressSink {
public:
  void update(std::string_view, double) override {}
};

class AnalysisPipeline {
public:
  Result<AnalysisResult, Error> run(const AnalysisRequest& request,
                                    ProgressSink& progress,
                                    std::stop_token stop = {});
};

}  // namespace cachescope
