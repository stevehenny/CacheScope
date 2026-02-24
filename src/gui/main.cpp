#define GLFW_INCLUDE_NONE

#include <GLFW/glfw3.h>

#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
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
    size_t index                = 0;
    std::string base_addr;
    size_t samples              = 0;
    size_t reads                = 0;
    size_t writes               = 0;
    size_t threads              = 0;
    size_t distinct_offsets     = 0;
    size_t shared_offsets       = 0;
    double private_offset_frac  = 0.0;
    size_t top_offsets          = 0;
    size_t thread_switches      = 0;
    double bounce_score         = 0.0;
    std::string min_addr;
    std::string max_addr;
    int64_t range_bytes         = 0;
  };

  std::vector<FalseSharingEntry> false_sharing;
};

struct JsonReader {
  std::string_view input;
  size_t pos = 0;

  void skip_ws() {
    while (pos < input.size() &&
           std::isspace(static_cast<unsigned char>(input[pos]))) {
      ++pos;
    }
  }

  bool consume(char c) {
    skip_ws();
    if (pos < input.size() && input[pos] == c) {
      ++pos;
      return true;
    }
    return false;
  }

  bool expect(char c, std::string& error) {
    if (consume(c)) return true;
    error = "Expected '";
    error.push_back(c);
    error += "'.";
    return false;
  }

  bool parse_string(std::string& out, std::string& error) {
    skip_ws();
    if (pos >= input.size() || input[pos] != '"') {
      error = "Expected string.";
      return false;
    }
    ++pos;
    out.clear();
    while (pos < input.size()) {
      char c = input[pos++];
      if (c == '"') return true;
      if (c == '\\') {
        if (pos >= input.size()) {
          error = "Invalid escape sequence.";
          return false;
        }
        char esc = input[pos++];
        switch (esc) {
          case '"':
          case '\\':
          case '/':
            out.push_back(esc);
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u': {
            if (pos + 4 > input.size()) {
              error = "Invalid unicode escape.";
              return false;
            }
            unsigned value = 0;
            for (int i = 0; i < 4; ++i) {
              char h = input[pos++];
              value <<= 4;
              if (h >= '0' && h <= '9') {
                value += static_cast<unsigned>(h - '0');
              } else if (h >= 'a' && h <= 'f') {
                value += static_cast<unsigned>(h - 'a' + 10);
              } else if (h >= 'A' && h <= 'F') {
                value += static_cast<unsigned>(h - 'A' + 10);
              } else {
                error = "Invalid unicode escape.";
                return false;
              }
            }
            out.push_back(value <= 0x7F ? static_cast<char>(value) : '?');
            break;
          }
          default:
            error = "Invalid escape sequence.";
            return false;
        }
      } else {
        out.push_back(c);
      }
    }
    error = "Unterminated string.";
    return false;
  }

  bool parse_number_token(std::string_view& out, std::string& error) {
    skip_ws();
    size_t start = pos;
    if (pos >= input.size()) {
      error = "Expected number.";
      return false;
    }
    char c = input[pos];
    if (!(c == '-' || std::isdigit(static_cast<unsigned char>(c)))) {
      error = "Expected number.";
      return false;
    }
    ++pos;
    while (pos < input.size()) {
      char ch = input[pos];
      if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.' ||
          ch == 'e' || ch == 'E' || ch == '+' || ch == '-') {
        ++pos;
      } else {
        break;
      }
    }
    out = input.substr(start, pos - start);
    return true;
  }

  bool parse_int64(int64_t& out, std::string& error) {
    std::string_view token;
    if (!parse_number_token(token, error)) return false;
    auto [ptr, ec] =
      std::from_chars(token.data(), token.data() + token.size(), out);
    if (ec != std::errc() || ptr != token.data() + token.size()) {
      error = "Invalid integer value.";
      return false;
    }
    return true;
  }

  bool parse_size_t(size_t& out, std::string& error) {
    int64_t value = 0;
    if (!parse_int64(value, error)) return false;
    if (value < 0 ||
        static_cast<uint64_t>(value) > std::numeric_limits<size_t>::max()) {
      error = "Value out of range.";
      return false;
    }
    out = static_cast<size_t>(value);
    return true;
  }

  bool parse_double(double& out, std::string& error) {
    std::string_view token;
    if (!parse_number_token(token, error)) return false;
    std::string temp(token);
    char* end = nullptr;
    out = std::strtod(temp.c_str(), &end);
    if (!end || end != temp.c_str() + temp.size()) {
      error = "Invalid number.";
      return false;
    }
    return true;
  }

  bool consume_literal(std::string_view literal) {
    skip_ws();
    if (input.substr(pos, literal.size()) == literal) {
      pos += literal.size();
      return true;
    }
    return false;
  }

  template <typename F>
  bool parse_object(F&& on_key, std::string& error) {
    if (!expect('{', error)) return false;
    skip_ws();
    if (consume('}')) return true;
    while (true) {
      std::string key;
      if (!parse_string(key, error)) return false;
      if (!expect(':', error)) return false;
      if (!on_key(key, error)) return false;
      skip_ws();
      if (consume('}')) break;
      if (!expect(',', error)) return false;
    }
    return true;
  }

  template <typename F>
  bool parse_array(F&& on_value, std::string& error) {
    if (!expect('[', error)) return false;
    skip_ws();
    if (consume(']')) return true;
    size_t index = 0;
    while (true) {
      if (!on_value(index, error)) return false;
      skip_ws();
      if (consume(']')) break;
      if (!expect(',', error)) return false;
      ++index;
    }
    return true;
  }

  bool skip_value(std::string& error) {
    skip_ws();
    if (pos >= input.size()) {
      error = "Unexpected end of input.";
      return false;
    }
    char c = input[pos];
    if (c == '{') {
      return parse_object(
        [&](const std::string&, std::string& err) { return skip_value(err); },
        error);
    }
    if (c == '[') {
      return parse_array(
        [&](size_t, std::string& err) { return skip_value(err); }, error);
    }
    if (c == '"') {
      std::string ignored;
      return parse_string(ignored, error);
    }
    if (c == 't' && consume_literal("true")) return true;
    if (c == 'f' && consume_literal("false")) return true;
    if (c == 'n' && consume_literal("null")) return true;
    std::string_view token;
    return parse_number_token(token, error);
  }
};

static bool parse_metadata(JsonReader& reader,
                           ReportData::Metadata& metadata,
                           std::string& error) {
  return reader.parse_object(
    [&](const std::string& key, std::string& err) {
      if (key == "binary") return reader.parse_string(metadata.binary, err);
      if (key == "event") return reader.parse_string(metadata.event, err);
      if (key == "sample_period") {
        int64_t value = 0;
        if (!reader.parse_int64(value, err)) return false;
        metadata.sample_period = static_cast<int>(value);
        return true;
      }
      return reader.skip_value(err);
    },
    error);
}

static bool parse_sample_stats(JsonReader& reader,
                               ReportData::SampleStats& stats,
                               std::string& error) {
  return reader.parse_object(
    [&](const std::string& key, std::string& err) {
      if (key == "total_samples") return reader.parse_size_t(stats.total_samples, err);
      if (key == "samples_with_address") return reader.parse_size_t(stats.samples_with_address, err);
      if (key == "samples_with_ip") return reader.parse_size_t(stats.samples_with_ip, err);
      if (key == "samples_with_sp") return reader.parse_size_t(stats.samples_with_sp, err);
      if (key == "samples_with_bp") return reader.parse_size_t(stats.samples_with_bp, err);
      if (key == "unique_threads") return reader.parse_size_t(stats.unique_threads, err);
      if (key == "unique_cpus") return reader.parse_size_t(stats.unique_cpus, err);
      return reader.skip_value(err);
    },
    error);
}

static bool parse_address_range(JsonReader& reader,
                                ReportData::FalseSharingEntry& entry,
                                std::string& error) {
  return reader.parse_object(
    [&](const std::string& key, std::string& err) {
      if (key == "min") return reader.parse_string(entry.min_addr, err);
      if (key == "max") return reader.parse_string(entry.max_addr, err);
      if (key == "bytes") return reader.parse_int64(entry.range_bytes, err);
      return reader.skip_value(err);
    },
    error);
}

static bool parse_false_sharing_entry(JsonReader& reader,
                                      ReportData::FalseSharingEntry& entry,
                                      std::string& error) {
  return reader.parse_object(
    [&](const std::string& key, std::string& err) {
      if (key == "index") return reader.parse_size_t(entry.index, err);
      if (key == "base_addr") return reader.parse_string(entry.base_addr, err);
      if (key == "samples") return reader.parse_size_t(entry.samples, err);
      if (key == "reads") return reader.parse_size_t(entry.reads, err);
      if (key == "writes") return reader.parse_size_t(entry.writes, err);
      if (key == "threads") return reader.parse_size_t(entry.threads, err);
      if (key == "distinct_offsets") return reader.parse_size_t(entry.distinct_offsets, err);
      if (key == "shared_offsets") return reader.parse_size_t(entry.shared_offsets, err);
      if (key == "private_offset_fraction") {
        return reader.parse_double(entry.private_offset_frac, err);
      }
      if (key == "top_offsets") return reader.parse_size_t(entry.top_offsets, err);
      if (key == "thread_switches") return reader.parse_size_t(entry.thread_switches, err);
      if (key == "bounce_score") return reader.parse_double(entry.bounce_score, err);
      if (key == "address_range") return parse_address_range(reader, entry, err);
      return reader.skip_value(err);
    },
    error);
}

static bool parse_false_sharing(JsonReader& reader,
                                std::vector<ReportData::FalseSharingEntry>& entries,
                                std::string& error) {
  entries.clear();
  return reader.parse_array(
    [&](size_t, std::string& err) {
      ReportData::FalseSharingEntry entry;
      if (!parse_false_sharing_entry(reader, entry, err)) return false;
      entries.push_back(entry);
      return true;
    },
    error);
}

static bool parse_report(std::string_view content,
                         ReportData& report,
                         std::string& error) {
  JsonReader reader{content};
  if (!reader.parse_object(
        [&](const std::string& key, std::string& err) {
          if (key == "metadata") return parse_metadata(reader, report.metadata, err);
          if (key == "sample_stats") return parse_sample_stats(reader, report.stats, err);
          if (key == "false_sharing") return parse_false_sharing(reader, report.false_sharing, err);
          return reader.skip_value(err);
        },
        error)) {
    return false;
  }
  reader.skip_ws();
  if (reader.pos != reader.input.size()) {
    error = "Unexpected trailing data.";
    return false;
  }
  return true;
}

static bool load_report(const std::string& path,
                        ReportData& report,
                        std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "Failed to open report file.";
    return false;
  }
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  if (content.empty() && in.fail()) {
    error = "Failed to read report file.";
    return false;
  }
  if (!parse_report(content, report, error)) {
    if (error.empty()) error = "Failed to parse report file.";
    return false;
  }
  return true;
}

static void render_report_tables(const ReportData& report) {
  const ImGuiTableFlags flags = ImGuiTableFlags_Borders |
                                ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_SizingStretchProp;

  ImGui::TextUnformatted("Metadata");
  ImGui::Separator();
  if (ImGui::BeginTable("metadata_table", 2, flags)) {
    ImGui::TableSetupColumn("Field");
    ImGui::TableSetupColumn("Value");
    ImGui::TableHeadersRow();
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("Binary");
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(report.metadata.binary.c_str());
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("Event");
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(report.metadata.event.c_str());
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("Sample period");
    ImGui::TableNextColumn();
    ImGui::Text("%d", report.metadata.sample_period);
    ImGui::EndTable();
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("Sample Statistics");
  ImGui::Separator();
  if (ImGui::BeginTable("stats_table", 2, flags)) {
    ImGui::TableSetupColumn("Metric");
    ImGui::TableSetupColumn("Value");
    ImGui::TableHeadersRow();
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("Total samples");
    ImGui::TableNextColumn();
    ImGui::Text("%zu", report.stats.total_samples);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("Samples with address");
    ImGui::TableNextColumn();
    ImGui::Text("%zu", report.stats.samples_with_address);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("Samples with IP");
    ImGui::TableNextColumn();
    ImGui::Text("%zu", report.stats.samples_with_ip);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("Samples with SP");
    ImGui::TableNextColumn();
    ImGui::Text("%zu", report.stats.samples_with_sp);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("Samples with BP");
    ImGui::TableNextColumn();
    ImGui::Text("%zu", report.stats.samples_with_bp);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("Unique threads");
    ImGui::TableNextColumn();
    ImGui::Text("%zu", report.stats.unique_threads);
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextUnformatted("Unique CPUs");
    ImGui::TableNextColumn();
    ImGui::Text("%zu", report.stats.unique_cpus);
    ImGui::EndTable();
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("False Sharing Analysis");
  ImGui::Separator();
  if (report.false_sharing.empty()) {
    ImGui::TextUnformatted("No hot cache lines detected.");
    return;
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("Summary");
  ImGui::Separator();
  if (ImGui::BeginTable("summary_table", 6, flags)) {
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("Base Address");
    ImGui::TableSetupColumn("Samples");
    ImGui::TableSetupColumn("Reads");
    ImGui::TableSetupColumn("Writes");
    ImGui::TableSetupColumn("Threads");
    ImGui::TableHeadersRow();
    for (const auto& entry : report.false_sharing) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%zu", entry.index);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(entry.base_addr.c_str());
      ImGui::TableNextColumn();
      ImGui::Text("%zu", entry.samples);
      ImGui::TableNextColumn();
      ImGui::Text("%zu", entry.reads);
      ImGui::TableNextColumn();
      ImGui::Text("%zu", entry.writes);
      ImGui::TableNextColumn();
      ImGui::Text("%zu", entry.threads);
    }
    ImGui::EndTable();
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("Offsets");
  ImGui::Separator();
  if (ImGui::BeginTable("offsets_table", 5, flags)) {
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("Distinct Offsets");
    ImGui::TableSetupColumn("Shared Offsets");
    ImGui::TableSetupColumn("Private Fraction");
    ImGui::TableSetupColumn("Top Offsets");
    ImGui::TableHeadersRow();
    for (const auto& entry : report.false_sharing) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%zu", entry.index);
      ImGui::TableNextColumn();
      ImGui::Text("%zu", entry.distinct_offsets);
      ImGui::TableNextColumn();
      ImGui::Text("%zu", entry.shared_offsets);
      ImGui::TableNextColumn();
      ImGui::Text("%.2f", entry.private_offset_frac);
      ImGui::TableNextColumn();
      ImGui::Text("%zu", entry.top_offsets);
    }
    ImGui::EndTable();
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("Bounce");
  ImGui::Separator();
  if (ImGui::BeginTable("bounce_table", 3, flags)) {
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("Thread Switches");
    ImGui::TableSetupColumn("Bounce Score");
    ImGui::TableHeadersRow();
    for (const auto& entry : report.false_sharing) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%zu", entry.index);
      ImGui::TableNextColumn();
      ImGui::Text("%zu", entry.thread_switches);
      ImGui::TableNextColumn();
      ImGui::Text("%.3f", entry.bounce_score);
    }
    ImGui::EndTable();
  }

  ImGui::Spacing();
  ImGui::TextUnformatted("Address Range");
  ImGui::Separator();
  if (ImGui::BeginTable("range_table", 4, flags)) {
    ImGui::TableSetupColumn("#");
    ImGui::TableSetupColumn("Min Address");
    ImGui::TableSetupColumn("Max Address");
    ImGui::TableSetupColumn("Range Bytes");
    ImGui::TableHeadersRow();
    for (const auto& entry : report.false_sharing) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("%zu", entry.index);
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(entry.min_addr.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(entry.max_addr.c_str());
      ImGui::TableNextColumn();
      ImGui::Text("%lld", static_cast<long long>(entry.range_bytes));
    }
    ImGui::EndTable();
  }
}

int main(int argc, char** argv) {
  std::string report_path;
  if (argc > 1) report_path = argv[1];

  std::string load_error;
  ReportData report;
  if (!report_path.empty()) {
    load_report(report_path, report, load_error);
  }

  if (!glfwInit()) {
    std::fprintf(stderr, "Failed to initialize GLFW\n");
    return 1;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

  GLFWwindow* window =
    glfwCreateWindow(1280, 720, "CacheScope GUI", nullptr, nullptr);
  if (!window) {
    std::fprintf(stderr, "Failed to create GLFW window\n");
    glfwTerminate();
    return 1;
  }

  glfwMakeContextCurrent(window);
  glfwSwapInterval(1);

  if (imgl3wInit() != 0) {
    std::fprintf(stderr, "Failed to initialize OpenGL loader (ImGui)\n");
    glfwDestroyWindow(window);
    glfwTerminate();
    return 1;
  }

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init("#version 460");

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGuiWindowFlags window_flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
      ImGuiWindowFlags_NoBringToFrontOnFocus |
      ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_HorizontalScrollbar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("CacheScope Report Viewer", nullptr, window_flags);
    ImGui::PopStyleVar(2);
    if (report_path.empty()) {
      ImGui::TextUnformatted("Usage: cache_scope_gui <report.json>");
    } else {
      ImGui::Text("Report: %s", report_path.c_str());
      if (!load_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                           load_error.c_str());
      } else {
        ImGui::Separator();
        render_report_tables(report);
      }
    }
    ImGui::End();

    ImGui::Render();
    int display_w = 0;
    int display_h = 0;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window);
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
