#define GLFW_INCLUDE_NONE

#include <GLFW/glfw3.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3_loader.h"
#include "imgui_impl_opengl3.h"

static std::string load_report(const std::string& path, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "Failed to open report file.";
    return {};
  }
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  if (content.empty() && in.fail()) {
    error = "Failed to read report file.";
  }
  return content;
}

int main(int argc, char** argv) {
  std::string report_path;
  if (argc > 1) report_path = argv[1];

  std::string load_error;
  std::string report_text;
  if (!report_path.empty()) {
    report_text = load_report(report_path, load_error);
  }

  if (!glfwInit()) {
    std::fprintf(stderr, "Failed to initialize GLFW\n");
    return 1;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

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
  ImGui_ImplOpenGL3_Init("#version 330");

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGui::Begin("CacheScope Report Viewer");
    if (report_path.empty()) {
      ImGui::TextUnformatted("Usage: cache_scope_gui <report.json>");
    } else {
      ImGui::Text("Report: %s", report_path.c_str());
      if (!load_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s",
                           load_error.c_str());
      } else {
        ImGui::Separator();
        ImGui::BeginChild("report_text", ImVec2(0.0f, 0.0f), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(report_text.c_str());
        ImGui::EndChild();
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
