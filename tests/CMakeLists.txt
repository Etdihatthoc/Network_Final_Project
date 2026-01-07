add_executable(codec_tests
  codec_tests.cpp
)

target_link_libraries(codec_tests
  PRIVATE
    common
    project_deps
)

add_test(NAME codec_tests COMMAND codec_tests)
