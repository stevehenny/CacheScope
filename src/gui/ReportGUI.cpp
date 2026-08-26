#include "gui/ReportGUI.hpp"

#include <cstdio>
#include "common/Format.hpp"
#include <string>

#include "report/JsonReport.hpp"

namespace {

constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_Borders |
                                        ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_SizingStretchProp;

const char* availability(bool available) {
  return available ? "available" : "unavailable";
}

void field_row(const char* name, const std::string& value) {
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextUnformatted(name);
  ImGui::TableNextColumn();
  ImGui::TextWrapped("%s", value.c_str());
}

void value_row(const char* name, std::uint64_t value) {
  field_row(name, std::to_string(value));
}

void render_metadata(const AnalysisResult& report) {
  if (!ImGui::BeginTable("metadata", 2, kTableFlags)) return;
  ImGui::TableSetupColumn("Field");
  ImGui::TableSetupColumn("Value");
  ImGui::TableHeadersRow();
  field_row("Report schema", report.schema_version);
  field_row("Tool version", report.capture.tool_version);
  field_row("Binary", report.metadata.binary);
  field_row("Event", report.metadata.event);
  field_row("Sample period", std::to_string(report.metadata.sample_rate));
  field_row("Kernel", report.capture.kernel_release);
  field_row("CPU", report.capture.cpu_model);
  field_row("Build ID", report.capture.target_build_id.empty()
                          ? "unavailable" : report.capture.target_build_id);
  field_row("Clock", report.capture.clock_source);
  ImGui::EndTable();
}

void render_capabilities(const AnalysisResult& report) {
  const auto& capability = report.capture.capabilities;
  if (ImGui::BeginTable("capabilities", 2, kTableFlags)) {
    ImGui::TableSetupColumn("Capability");
    ImGui::TableSetupColumn("Status");
    ImGui::TableHeadersRow();
    field_row("CPU vendor", capability.cpu_vendor);
    field_row("Perf events", availability(capability.perf_events));
    field_row("Intel PEBS", availability(capability.intel_pebs));
    field_row("AMD IBS", availability(capability.amd_ibs));
    field_row("Physical addresses",
              availability(capability.physical_addresses));
    field_row("User registers", availability(capability.user_registers));
    ImGui::EndTable();
  }
  for (const auto& unavailable : capability.unavailable) {
    ImGui::BulletText("%s", unavailable.c_str());
  }
}

void render_quality(const AnalysisResult& report) {
  if (ImGui::BeginTable("quality", 2, kTableFlags)) {
    ImGui::TableSetupColumn("Metric");
    ImGui::TableSetupColumn("Value");
    ImGui::TableHeadersRow();
    value_row("Trace samples", report.quality.samples);
    value_row("Lost records", report.quality.lost);
    value_row("Throttle records", report.quality.throttled);
    value_row("Malformed records", report.quality.malformed_records);
    value_row("Unknown records", report.quality.unknown_records);
    value_row("Evicted by cap", report.quality.evicted_samples);
    field_row("Truncated", report.quality.truncated ? "yes" : "no");
    field_row("Capture completed", report.quality.completed ? "yes" : "no");
    ImGui::EndTable();
  }

  for (const auto& diagnostic : report.diagnostics) {
    const ImVec4 color =
      diagnostic.severity == "warning"
        ? ImVec4(1.0f, 0.75f, 0.20f, 1.0f)
        : ImVec4(0.65f, 0.75f, 1.0f, 1.0f);
    ImGui::TextColored(color, "%s [%s]", diagnostic.severity.c_str(),
                       diagnostic.code.c_str());
    ImGui::SameLine();
    ImGui::TextWrapped("%s", diagnostic.message.c_str());
    if (!diagnostic.remediation.empty()) {
      ImGui::Indent();
      ImGui::TextWrapped("Remediation: %s",
                         diagnostic.remediation.c_str());
      ImGui::Unindent();
    }
  }
}

void render_stats(const AnalysisResult& report) {
  if (!ImGui::BeginTable("sample_stats", 2, kTableFlags)) return;
  ImGui::TableSetupColumn("Metric");
  ImGui::TableSetupColumn("Value");
  ImGui::TableHeadersRow();
  value_row("Total samples", report.stats.total_samples);
  value_row("Samples with address", report.stats.samples_with_addr);
  value_row("Samples with physical address",
            report.stats.samples_with_phys_addr);
  value_row("Samples with IP", report.stats.samples_with_ip);
  value_row("Samples with SP", report.stats.samples_with_sp);
  value_row("Samples with BP", report.stats.samples_with_bp);
  value_row("Unique threads", report.stats.unique_threads);
  value_row("Unique CPUs", report.stats.unique_cpus);
  ImGui::EndTable();
}

void render_attribution(
  const std::vector<cachescope::FindingAttribution>& attribution) {
  if (attribution.empty()) {
    ImGui::TextDisabled("Attribution unavailable");
    return;
  }
  for (const auto& item : attribution) {
    ImGui::BulletText("%s %s %s (%llu samples, confidence %.3f)",
                      item.scope.c_str(), item.variable.c_str(),
                      item.field_path.c_str(),
                      static_cast<unsigned long long>(item.sample_count),
                      item.confidence);
    ImGui::Indent();
    if (!item.type.empty()) ImGui::Text("Type: %s", item.type.c_str());
    if (!item.tids.empty()) {
      std::string tids;
      for (const auto tid : item.tids) {
        if (!tids.empty()) tids += ", ";
        tids += std::to_string(tid);
      }
      ImGui::Text("TIDs: %s", tids.c_str());
    }
    for (const auto& evidence : item.evidence) {
      ImGui::TextWrapped("Evidence: %s", evidence.c_str());
    }
    ImGui::Unindent();
  }
}

void render_false_sharing(const AnalysisResult& report) {
  if (report.false_sharing.empty()) {
    ImGui::TextUnformatted("No suspected false-sharing findings.");
    return;
  }

  if (ImGui::BeginTable("false_sharing", 8, kTableFlags)) {
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("Cache line");
    ImGui::TableSetupColumn("Samples");
    ImGui::TableSetupColumn("Threads");
    ImGui::TableSetupColumn("Offsets");
    ImGui::TableSetupColumn("Bounce");
    ImGui::TableSetupColumn("Private");
    ImGui::TableSetupColumn("Confidence");
    ImGui::TableHeadersRow();
    for (const auto& finding : report.false_sharing) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%zu", finding.index);
      ImGui::TableNextColumn();
      ImGui::Text("0x%llx",
                  static_cast<unsigned long long>(finding.base_addr));
      ImGui::TableNextColumn();
      ImGui::Text("%zu", finding.sample_count);
      ImGui::TableNextColumn();
      ImGui::Text("%zu", finding.unique_threads);
      ImGui::TableNextColumn();
      ImGui::Text("%zu", finding.distinct_offsets);
      ImGui::TableNextColumn();
      ImGui::Text("%.3f", finding.bounce_score);
      ImGui::TableNextColumn();
      ImGui::Text("%.3f", finding.private_offset_fraction);
      ImGui::TableNextColumn();
      ImGui::Text("%.3f", finding.confidence);
    }
    ImGui::EndTable();
  }

  for (const auto& finding : report.false_sharing) {
    const auto label = cachescope::format(
      "Finding #{}: {} (confidence {:.3f})", finding.index,
      finding.suspected_cause, finding.confidence);
    if (ImGui::TreeNode(label.c_str())) {
      ImGui::Text("Reads: %zu; writes: %zu; thread switches: %zu",
                  finding.sample_reads, finding.sample_writes,
                  finding.thread_switches);
      ImGui::Text("Address range: 0x%llx - 0x%llx (%lld bytes)",
                  static_cast<unsigned long long>(finding.min_addr),
                  static_cast<unsigned long long>(finding.max_addr),
                  static_cast<long long>(finding.range_bytes));
      render_attribution(finding.attribution);
      ImGui::TreePop();
    }
  }
}

void render_topology(const AnalysisResult& report) {
  if (!ImGui::BeginTable("cache_topology", 9, kTableFlags)) return;
  ImGui::TableSetupColumn("Level");
  ImGui::TableSetupColumn("Type");
  ImGui::TableSetupColumn("ID");
  ImGui::TableSetupColumn("Size KiB");
  ImGui::TableSetupColumn("Line");
  ImGui::TableSetupColumn("Sets");
  ImGui::TableSetupColumn("Ways");
  ImGui::TableSetupColumn("Shared CPUs");
  ImGui::TableSetupColumn("Source");
  ImGui::TableHeadersRow();
  for (const auto& cache : report.cache_topology) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("L%d", cache.level);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(cache.type.c_str());
    ImGui::TableNextColumn();
    ImGui::Text("%d", cache.id);
    ImGui::TableNextColumn();
    ImGui::Text("%zu", cache.size_bytes / 1024);
    ImGui::TableNextColumn();
    ImGui::Text("%zu", cache.line_size);
    ImGui::TableNextColumn();
    ImGui::Text("%zu", cache.sets);
    ImGui::TableNextColumn();
    ImGui::Text("%zu", cache.associativity);
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(cache.shared_cpu_list.c_str());
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(cache.detected_from_sysfs ? "sysfs" : "fallback");
  }
  ImGui::EndTable();
}

void render_thrashing(const AnalysisResult& report) {
  if (report.cache_thrashing.empty()) {
    ImGui::TextUnformatted("No suspected cache-thrashing findings.");
    return;
  }

  if (ImGui::BeginTable("thrashing", 9, kTableFlags)) {
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("Cache");
    ImGui::TableSetupColumn("Set");
    ImGui::TableSetupColumn("Basis");
    ImGui::TableSetupColumn("Samples");
    ImGui::TableSetupColumn("Lines");
    ImGui::TableSetupColumn("Reloads");
    ImGui::TableSetupColumn("Score");
    ImGui::TableSetupColumn("Confidence");
    ImGui::TableHeadersRow();
    for (const auto& finding : report.cache_thrashing) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%zu", finding.index);
      ImGui::TableNextColumn();
      ImGui::Text("L%d %s %d", finding.cache_level,
                  finding.cache_type.c_str(), finding.cache_id);
      ImGui::TableNextColumn();
      ImGui::Text("%zu", finding.cache_set);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(finding.address_basis.c_str());
      ImGui::TableNextColumn();
      ImGui::Text("%zu", finding.sample_count);
      ImGui::TableNextColumn();
      ImGui::Text("%zu", finding.unique_lines);
      ImGui::TableNextColumn();
      ImGui::Text("%zu", finding.eviction_reloads);
      ImGui::TableNextColumn();
      ImGui::Text("%.3f", finding.score);
      ImGui::TableNextColumn();
      ImGui::Text("%.3f", finding.confidence);
    }
    ImGui::EndTable();
  }

  for (const auto& finding : report.cache_thrashing) {
    const auto label = cachescope::format(
      "Episode #{}: {} (confidence {:.3f})", finding.index,
      finding.suspected_cause, finding.confidence);
    if (ImGui::TreeNode(label.c_str())) {
      ImGui::Text("CPUs: %s; threads: %zu; duration: %lld ns",
                  finding.shared_cpu_list.c_str(), finding.unique_threads,
                  static_cast<long long>(finding.duration_ns));
      ImGui::Text("Evictions: %zu; reload ratio: %.3f; pressure: %.2fx",
                  finding.evictions, finding.reload_ratio,
                  finding.oversubscription);
      render_attribution(finding.attribution);
      ImGui::TreePop();
    }
  }
}

}  // namespace

ReportGUI::ReportGUI() = default;

ReportGUI::~ReportGUI() {
  if (!initialized_) return;
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  if (window_) glfwDestroyWindow(window_);
  glfwTerminate();
}

bool ReportGUI::init(std::string& error) {
  if (initialized_) return true;
  if (!glfwInit()) {
    error = "Failed to initialize GLFW.";
    return false;
  }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  window_ = glfwCreateWindow(1280, 720, "CacheScope Report", nullptr, nullptr);
  if (!window_) {
    error = "Failed to create GLFW window.";
    glfwTerminate();
    return false;
  }
  glfwMakeContextCurrent(window_);
  glfwSwapInterval(1);
  if (imgl3wInit() != 0) {
    error = "Failed to initialize OpenGL loader.";
    glfwDestroyWindow(window_);
    window_ = nullptr;
    glfwTerminate();
    return false;
  }
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForOpenGL(window_, true);
  ImGui_ImplOpenGL3_Init("#version 460");
  initialized_ = true;
  return true;
}

bool ReportGUI::load_report(const std::string& path, std::string& error) {
  auto loaded = JsonReport::read_file(path);
  if (!loaded) {
    error = loaded.error().code + ": " + loaded.error().message;
    if (loaded.error().system_error) {
      error += ": " + loaded.error().system_error.message();
    }
    return false;
  }
  report_ = std::move(loaded.value());
  return true;
}

void ReportGUI::render_report(const AnalysisResult& report) {
  if (ImGui::CollapsingHeader("Metadata", ImGuiTreeNodeFlags_DefaultOpen)) {
    render_metadata(report);
  }
  if (ImGui::CollapsingHeader("Capabilities", ImGuiTreeNodeFlags_DefaultOpen)) {
    render_capabilities(report);
  }
  if (ImGui::CollapsingHeader("Sample Quality",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    render_quality(report);
  }
  if (ImGui::CollapsingHeader("Sample Statistics")) render_stats(report);
  if (ImGui::CollapsingHeader("False Sharing",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    render_false_sharing(report);
  }
  if (ImGui::CollapsingHeader("Cache Topology")) render_topology(report);
  if (ImGui::CollapsingHeader("Cache Thrashing",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    render_thrashing(report);
  }
}

void ReportGUI::render(const std::string& report_path) {
  if (!initialized_) {
    std::fprintf(stderr, "ReportGUI not initialized\n");
    return;
  }
  report_path_ = report_path;
  load_error_.clear();
  report_loaded_ = load_report(report_path_, load_error_);

  while (!glfwWindowShouldClose(window_)) {
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoBringToFrontOnFocus |
      ImGuiWindowFlags_NoNavFocus |
      ImGuiWindowFlags_HorizontalScrollbar;
    ImGui::Begin("CacheScope Report Viewer", nullptr, flags);
    ImGui::Text("Report: %s", report_path_.c_str());
    if (!load_error_.empty()) {
      ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                         load_error_.c_str());
    } else if (report_loaded_) {
      ImGui::Separator();
      render_report(report_);
    }
    ImGui::End();

    ImGui::Render();
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window_, &width, &height);
    glViewport(0, 0, width, height);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window_);
  }
}
