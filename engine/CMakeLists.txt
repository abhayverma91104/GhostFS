cmake_minimum_required(VERSION 3.20)
project(ghostfs_engine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json googletest)

add_library(ghostfs_core STATIC
  src/allocator/allocator.cpp
  src/allocator/comparison.cpp
  src/allocator/fragmentation.cpp
  src/common/base64.cpp
  src/common/crc32.cpp
  src/common/engine.cpp
  src/common/sha256.cpp
  src/disk/virtual_disk.cpp
  src/filesystem/gfs_format.cpp
  src/filesystem/gfs_fs.cpp
  src/hexviewer/binary_parser.cpp
  src/hexviewer/hex_inspector.cpp
  src/recovery/carver.cpp
  src/recovery/metadata_recovery.cpp
  src/recovery/recovery_engine.cpp
  src/repair/corruption.cpp
  src/repair/fsck.cpp
  src/repair/repair.cpp
  src/scheduling/disk_scheduler.cpp
  src/scheduling/io_trace.cpp
)

target_include_directories(ghostfs_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(ghostfs_core
  PUBLIC
    nlohmann_json::nlohmann_json
)

if(MSVC)
  target_compile_options(ghostfs_core PRIVATE /W4)
else()
  target_compile_options(ghostfs_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(ghostfs_engine
  src/cli/main.cpp
)

target_link_libraries(ghostfs_engine
  PRIVATE
    ghostfs_core
)

enable_testing()
include(GoogleTest)

add_executable(ghostfs_tests
  tests/test_smoke.cpp
  tests/test_phase2_disk_fs.cpp
  tests/test_phase3_allocator.cpp
  tests/test_phase4_recovery.cpp
  tests/test_phase5_hex.cpp
  tests/test_phase6_repair.cpp
  tests/test_phase7_scheduling.cpp
  tests/test_phase9_adversarial.cpp
)

target_link_libraries(ghostfs_tests
  PRIVATE
    ghostfs_core
    GTest::gtest_main
)

gtest_discover_tests(ghostfs_tests DISCOVERY_TIMEOUT 30)
