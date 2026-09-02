cmake_minimum_required(VERSION 3.16)
project(adocx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

option(ADOCX_ENABLE_ROCKSDB "Build with RocksDB backend" ON)

add_library(adocx
  src/adoc_db.cpp
)

target_include_directories(adocx
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(ADOCX_ENABLE_ROCKSDB)
  find_path(ROCKSDB_INCLUDE_DIR rocksdb/db.h)
  find_library(ROCKSDB_LIBRARY rocksdb)
  if(ROCKSDB_INCLUDE_DIR AND ROCKSDB_LIBRARY)
    target_compile_definitions(adocx PUBLIC ADOCX_HAS_ROCKSDB=1)
    target_include_directories(adocx PUBLIC ${ROCKSDB_INCLUDE_DIR})
    target_link_libraries(adocx PUBLIC ${ROCKSDB_LIBRARY})
  else()
    message(WARNING "RocksDB not found. Building memory-backend only. Set ROCKSDB paths or disable ADOCX_ENABLE_ROCKSDB.")
  endif()
endif()

add_executable(adocx_tests tests/test_adoc.cpp)
target_link_libraries(adocx_tests PRIVATE adocx)
add_test(NAME adocx_tests COMMAND adocx_tests)

add_executable(adocx_benchmark benchmarks/adoc_benchmark.cpp)
target_link_libraries(adocx_benchmark PRIVATE adocx)
