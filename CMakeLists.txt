cmake_minimum_required(VERSION 3.16)
project(adocrocks LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Build type ────────────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ── Compiler flags ────────────────────────────────────────────────────────────
add_compile_options(-Wall -Wextra -Wpedantic)

# ── RocksDB ───────────────────────────────────────────────────────────────────
# Search order:
#  1. CMAKE_PREFIX_PATH / environment hints (covers Homebrew user prefix)
#  2. Common system paths (Linux apt, /usr/local)
#  3. Homebrew default prefixes on Apple Silicon and Intel Mac

# Hint paths so find_* picks up user-prefix Homebrew installs
list(APPEND CMAKE_PREFIX_PATH
    "$ENV{HOME}/.homebrew"
    "/opt/homebrew"
    "/usr/local"
)

find_library(ROCKSDB_LIB
    NAMES rocksdb
    HINTS
        "$ENV{HOME}/.homebrew/lib"
        "/opt/homebrew/lib"
        "/usr/local/lib"
        "/usr/lib"
        "/usr/lib/x86_64-linux-gnu"
)
find_path(ROCKSDB_INCLUDE
    NAMES rocksdb/db.h
    HINTS
        "$ENV{HOME}/.homebrew/include"
        "/opt/homebrew/include"
        "/usr/local/include"
        "/usr/include"
)

if(ROCKSDB_LIB AND ROCKSDB_INCLUDE)
  set(ROCKSDB_FOUND TRUE)
  set(ROCKSDB_LIBRARIES ${ROCKSDB_LIB})
  set(ROCKSDB_INCLUDE_DIRS ${ROCKSDB_INCLUDE})
else()
  message(FATAL_ERROR
    "RocksDB not found.\n"
    "  macOS:  brew install rocksdb\n"
    "  Ubuntu: sudo apt install librocksdb-dev")
endif()

message(STATUS "RocksDB include: ${ROCKSDB_INCLUDE_DIRS}")
message(STATUS "RocksDB libs:    ${ROCKSDB_LIBRARIES}")

# ── System deps (needed by RocksDB when linking statically) ──────────────────
find_package(Threads REQUIRED)
find_package(ZLIB REQUIRED)

# Optional compression libs – find them if present (needed on macOS where
# the Homebrew RocksDB bottle links against them).
find_library(SNAPPY_LIB  NAMES snappy
    HINTS "$ENV{HOME}/.homebrew/lib" "/opt/homebrew/lib" "/usr/lib" "/usr/local/lib")
find_library(LZ4_LIB     NAMES lz4
    HINTS "$ENV{HOME}/.homebrew/lib" "/opt/homebrew/lib" "/usr/lib" "/usr/local/lib")
find_library(ZSTD_LIB    NAMES zstd
    HINTS "$ENV{HOME}/.homebrew/lib" "/opt/homebrew/lib" "/usr/lib" "/usr/local/lib")

set(COMPRESSION_LIBS "")
if(SNAPPY_LIB)
  list(APPEND COMPRESSION_LIBS ${SNAPPY_LIB})
endif()
if(LZ4_LIB)
  list(APPEND COMPRESSION_LIBS ${LZ4_LIB})
endif()
if(ZSTD_LIB)
  list(APPEND COMPRESSION_LIBS ${ZSTD_LIB})
endif()
message(STATUS "Compression libs: ${COMPRESSION_LIBS}")

# ── Sources for the main binary ───────────────────────────────────────────────
set(ADOC_SRCS
    src/metrics_collector.cpp
    src/stall_listener.cpp
    src/adoc_tuner.cpp
    src/workload.cpp
    src/main.cpp
)

add_executable(adocrocks ${ADOC_SRCS})

target_include_directories(adocrocks PRIVATE
    ${ROCKSDB_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(adocrocks PRIVATE
    ${ROCKSDB_LIBRARIES}
    Threads::Threads
    ZLIB::ZLIB
    ${COMPRESSION_LIBS}
)

# ── GoogleTest ───────────────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(adoc_tests
    tests/test_adoc.cpp
    src/adoc_tuner.cpp
    src/workload.cpp
    src/stall_listener.cpp
    src/metrics_collector.cpp
)

target_include_directories(adoc_tests PRIVATE
    ${ROCKSDB_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(adoc_tests PRIVATE
    GTest::gtest_main
    ${ROCKSDB_LIBRARIES}
    Threads::Threads
    ZLIB::ZLIB
    ${COMPRESSION_LIBS}
)

include(GoogleTest)
gtest_discover_tests(adoc_tests)
