#pragma once
#define GLFW_INCLUDE_NONE

#include <GLFW/glfw3.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_opengl3_loader.h"

struct ReportData {
  struct Metadata {
    std::string binary;
    std::string event;
    int sample_period = 0;
  } metadata;

  struct SampleStats {
    size_t total_samples        = 0;
    size_t samples_with_address = 0;
    size_t samples_with_ip      = 0;
    size_t samples_with_sp      = 0;
    size_t samples_with_bp      = 0;
    size_t unique_threads       = 0;
    size_t unique_cpus          = 0;
  } stats;

  struct FalseSharingEntry {
    size_t index = 0;
    std::string base_addr;
    size_t samples             = 0;
    size_t reads               = 0;
    size_t writes              = 0;
    size_t threads             = 0;
    size_t distinct_offsets    = 0;
    size_t shared_offsets      = 0;
    double private_offset_frac = 0.0;
    size_t top_offsets         = 0;
    size_t thread_switches     = 0;
    double bounce_score        = 0.0;
    std::string min_addr;
    std::string max_addr;
    int64_t range_bytes = 0;
  };

  std::vector<FalseSharingEntry> false_sharing;
};

class ReportGUI {
private:
  GLFWwindow* window_ = nullptr;
  bool initialized_   = false;
  std::string report_path_;
  std::string load_error_;
  ReportData report_;
  bool report_loaded_ = false;

  bool load_report(const std::string& path, std::string& error);
  void render_report(const ReportData& report);

public:
  ReportGUI();
  ~ReportGUI();

  bool init(std::string& error);
  void render(const std::string& report_path);
};
