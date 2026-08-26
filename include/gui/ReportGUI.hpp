#pragma once
#define GLFW_INCLUDE_NONE

#include <GLFW/glfw3.h>

#include <string>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_opengl3_loader.h"
#include "report/Report.hpp"

class ReportGUI {
private:
  GLFWwindow* window_ = nullptr;
  bool initialized_ = false;
  std::string report_path_;
  std::string load_error_;
  AnalysisResult report_;
  bool report_loaded_ = false;

  bool load_report(const std::string& path, std::string& error);
  void render_report(const AnalysisResult& report);

public:
  ReportGUI();
  ~ReportGUI();

  bool init(std::string& error);
  void render(const std::string& report_path);
};
