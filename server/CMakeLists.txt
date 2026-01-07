add_executable(server_app
  src/main.cpp
  src/auth.cpp
  src/room.cpp
  src/server.cpp
  src/thread_pool.cpp
)

target_include_directories(server_app
  PRIVATE
    ${PROJECT_SOURCE_DIR}/server/include
    ${PROJECT_SOURCE_DIR}/common/include
)

target_link_libraries(server_app
  PRIVATE
    common
    project_deps
)

set_target_properties(server_app PROPERTIES OUTPUT_NAME "server")
