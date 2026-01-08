# Find OpenSSL for AES encryption
find_package(OpenSSL REQUIRED)

add_library(common STATIC
  src/codec.cpp
  src/crypto.cpp
  src/aes_crypto.cpp
  src/placeholder.cpp
)

target_include_directories(common
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(common
  PUBLIC
    project_deps
    OpenSSL::Crypto
)
