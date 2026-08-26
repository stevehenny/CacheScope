set(IMGUI_DIR ${PROJECT_SOURCE_DIR}/third_party/imgui)

if (NOT EXISTS ${IMGUI_DIR}/imgui.cpp)
  message(WARNING "ImGui submodule not found at ${IMGUI_DIR}. Run: git submodule update --init --recursive")
else()
  find_package(OpenGL REQUIRED)
  find_package(glfw3 3.3 REQUIRED)

  add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
  )
add_library(gui
  ReportGUI.cpp
)

target_link_libraries(gui
  PUBLIC
    cache_scope_includes
    report
    cachescope_project_options
    cachescope_warnings
    imgui
)

  target_include_directories(gui
    PUBLIC
      ${IMGUI_DIR}
      ${IMGUI_DIR}/backends
  )

  target_link_libraries(gui PUBLIC glfw OpenGL::GL)

  target_include_directories(imgui
    PUBLIC
      ${IMGUI_DIR}
      ${IMGUI_DIR}/backends
  )
  target_link_libraries(imgui PUBLIC glfw OpenGL::GL)
  if (UNIX AND NOT APPLE)
    target_link_libraries(imgui PUBLIC dl)
  endif()

endif()
