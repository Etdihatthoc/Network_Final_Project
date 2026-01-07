cmake_minimum_required(VERSION 3.16)
project(online_quiz LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(BUILD_SERVER "Build the server binary" ON)
option(BUILD_CLIENT "Build the client binary" ON)
option(BUILD_TESTS  "Build test targets" ON)
option(USE_FETCHCONTENT_DEPS "Download dependencies via FetchContent" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/Deps.cmake)

add_subdirectory(common)
add_subdirectory(scripts)
if(BUILD_SERVER)
  add_subdirectory(server)
endif()
if(BUILD_CLIENT)
  add_subdirectory(client)
endif()

include(CTest)
if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
  add_subdirectory(tests)
endif()
