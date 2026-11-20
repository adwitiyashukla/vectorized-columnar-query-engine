cmake_minimum_required(VERSION 3.16)
project(vcqe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(VCQE_BUILD_TESTS "Build the test binaries" ON)
option(VCQE_ENABLE_AVX2 "Compile the AVX2 kernels" ON)
option(VCQE_ASAN "Build with address and undefined behavior sanitizers" OFF)
option(VCQE_TSAN "Build with the thread sanitizer" OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /EHsc /utf-8)
  add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(VCQE_ASAN)
  if(MSVC)
    add_compile_options(/fsanitize=address)
  else()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

if(VCQE_TSAN AND NOT MSVC)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

set(VCQE_SOURCES
  src/config.cpp
  src/csv.cpp
  src/engine.cpp
  src/expr.cpp
  src/gen.cpp
  src/hash_table.cpp
  src/kernels.cpp
  src/operator.cpp
  src/parser.cpp
  src/plan.cpp
  src/reader.cpp
  src/row_exec.cpp
  src/scheduler.cpp
  src/types.cpp
  src/util.cpp
  src/writer.cpp
)

add_library(vcqe_core STATIC ${VCQE_SOURCES})
target_include_directories(vcqe_core PUBLIC src)
find_package(Threads REQUIRED)
target_link_libraries(vcqe_core PUBLIC Threads::Threads)

if(VCQE_ENABLE_AVX2)
  target_sources(vcqe_core PRIVATE src/kernels_avx2.cpp)
  target_compile_definitions(vcqe_core PRIVATE VCQE_HAVE_AVX2=1)
  if(MSVC)
    set_source_files_properties(src/kernels_avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
  else()
    set_source_files_properties(src/kernels_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2")
  endif()
endif()

add_executable(vcqe src/main.cpp)
target_link_libraries(vcqe PRIVATE vcqe_core)

if(VCQE_BUILD_TESTS)
  enable_testing()
  foreach(name config_test storage_test vector_test sql_test exec_test corruption_test)
    add_executable(${name} tests/${name}.cpp)
    target_link_libraries(${name} PRIVATE vcqe_core)
    target_include_directories(${name} PRIVATE tests)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  endforeach()
endif()
