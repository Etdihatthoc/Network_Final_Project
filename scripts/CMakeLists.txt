add_executable(seed_db
  seed.cpp
)

target_include_directories(seed_db
  PRIVATE
    ${PROJECT_SOURCE_DIR}/common/include
)

target_link_libraries(seed_db
  PRIVATE
    common
    project_deps
)

set_target_properties(seed_db PROPERTIES OUTPUT_NAME "seed_db")

add_executable(echo_client
  echo_client.cpp
)

target_include_directories(echo_client
  PRIVATE
    ${PROJECT_SOURCE_DIR}/common/include
)

target_link_libraries(echo_client
  PRIVATE
    common
    project_deps
)

set_target_properties(echo_client PROPERTIES OUTPUT_NAME "echo_client")
