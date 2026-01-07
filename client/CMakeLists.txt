find_package(SDL2 REQUIRED)

add_library(imgui STATIC
  ${PROJECT_SOURCE_DIR}/third_party/imgui/imgui.cpp
  ${PROJECT_SOURCE_DIR}/third_party/imgui/imgui_draw.cpp
  ${PROJECT_SOURCE_DIR}/third_party/imgui/imgui_tables.cpp
  ${PROJECT_SOURCE_DIR}/third_party/imgui/imgui_widgets.cpp
  ${PROJECT_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_sdl2.cpp
  ${PROJECT_SOURCE_DIR}/third_party/imgui/backends/imgui_impl_sdlrenderer2.cpp
)

target_include_directories(imgui PUBLIC
  ${PROJECT_SOURCE_DIR}/third_party/imgui
  ${PROJECT_SOURCE_DIR}/third_party/imgui/backends
  ${SDL2_INCLUDE_DIRS}
)

add_executable(client_app
  src/main.cpp
  src/core.cpp
  src/ui.cpp
)

target_include_directories(client_app
  PRIVATE
    ${PROJECT_SOURCE_DIR}/client/include
    ${PROJECT_SOURCE_DIR}/common/include
)

target_link_libraries(client_app
  PRIVATE
    common
    project_deps
    imgui
    SDL2::SDL2
)

set_target_properties(client_app PROPERTIES OUTPUT_NAME "client")
