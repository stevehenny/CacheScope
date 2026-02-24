#pragma once
#define GLFW_INCLUDE_NONE

#include <GLFW/glfw3.h>

#include <string>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_opengl3_loader.h"

using std::string;
class ReportGUI {
public:
  ReportGUI();
  ~ReportGUI();

  bool init(string& error);
  void run(const string& report_text);
};
