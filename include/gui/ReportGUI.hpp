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
    size_t samples_with_physical_address = 0;
    size_t samples_with_ip      = 0;
    size_t samples_with_sp      = 0;
    size_t samples_with_bp      = 0;
    size_t unique_threads       = 0;
    size_t unique_cpus          = 0;
  } stats;

  struct CacheInfo {
    int level              = 0;
    int id                 = 0;
    std::string type;
    size_t size_bytes      = 0;
    size_t line_size       = 0;
    size_t sets            = 0;
    size_t ways            = 0;
    std::string shared_cpu_list;
    bool detected = false;
  };

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

  struct ThrashingEntry {
    size_t index = 0;
    int cache_level = 0;
    int cache_id = 0;
    std::string cache_type;
    std::string shared_cpu_list;
    std::string address_basis;
    size_t cache_set = 0;
    int64_t start_time_ns = 0;
    int64_t end_time_ns = 0;
    int64_t duration_ns = 0;
    size_t samples = 0;
    size_t unique_lines = 0;
    size_t evictions = 0;
    size_t eviction_reloads = 0;
    size_t threads = 0;
    size_t cpus = 0;
    double reload_ratio = 0.0;
    double oversubscription = 0.0;
    double score = 0.0;
  };

  std::vector<CacheInfo> cache_topology;
  std::vector<FalseSharingEntry> false_sharing;
  std::vector<ThrashingEntry> cache_thrashing;
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
