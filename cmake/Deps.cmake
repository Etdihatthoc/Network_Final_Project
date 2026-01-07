include_guard(GLOBAL)

# Optional: fetch dependencies from the network when enabled.
if(USE_FETCHCONTENT_DEPS)
  include(FetchContent)

  if(NOT TARGET nlohmann_json)
    FetchContent_Declare(
      nlohmann_json
      GIT_REPOSITORY https://github.com/nlohmann/json.git
      GIT_TAG v3.11.3
      GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
  endif()

  if(NOT TARGET spdlog)
    FetchContent_Declare(
      spdlog
      GIT_REPOSITORY https://github.com/gabime/spdlog.git
      GIT_TAG v1.12.0
      GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(spdlog)
  endif()

  if(NOT TARGET asio)
    FetchContent_Declare(
      asio
      GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
      GIT_TAG asio-1-28-0
      GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(asio)
  endif()
endif()

set(_project_third_party "${PROJECT_SOURCE_DIR}/third_party")

if(NOT TARGET nlohmann_json)
  add_library(nlohmann_json INTERFACE)
  target_include_directories(nlohmann_json INTERFACE "${_project_third_party}/nlohmann_json/include")
endif()

if(NOT TARGET spdlog)
  add_library(spdlog INTERFACE)
  target_include_directories(spdlog INTERFACE "${_project_third_party}/spdlog/include")
endif()

if(NOT TARGET asio)
  add_library(asio INTERFACE)
  target_include_directories(asio INTERFACE "${_project_third_party}/asio/include")
  target_compile_definitions(asio INTERFACE ASIO_STANDALONE)
endif()

find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
  add_library(sqlite3_external INTERFACE)
  target_link_libraries(sqlite3_external INTERFACE SQLite::SQLite3)
else()
  add_library(sqlite3_external INTERFACE)
  target_include_directories(sqlite3_external INTERFACE "${_project_third_party}/sqlite/include")
endif()

add_library(project_deps INTERFACE)
target_link_libraries(project_deps
  INTERFACE
    nlohmann_json
    spdlog
    asio
    sqlite3_external
)
