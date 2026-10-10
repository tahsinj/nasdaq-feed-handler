cmake_minimum_required(VERSION 3.20)
project(nasdaq_feed_handler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(FH_NATIVE "Tune code for the CPU of the build machine" ON)
option(FH_BUILD_TESTS "Build the tests" ON)
set(FH_SANITIZE "" CACHE STRING "Sanitizers to enable, for example address,undefined or thread")

find_package(Threads REQUIRED)

add_library(fh_options INTERFACE)
if(MSVC)
  target_compile_options(fh_options INTERFACE /W4 /permissive- /Zc:__cplusplus /EHsc /wd4324)
  target_compile_definitions(fh_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN
                                                  _CRT_SECURE_NO_WARNINGS)
  if(FH_NATIVE)
    target_compile_options(fh_options INTERFACE /arch:AVX2)
  endif()
else()
  target_compile_options(fh_options INTERFACE -Wall -Wextra -Wpedantic -Wshadow)
  if(FH_NATIVE)
    target_compile_options(fh_options INTERFACE -march=native)
  endif()
  if(FH_SANITIZE)
    target_compile_options(fh_options INTERFACE -fsanitize=${FH_SANITIZE}
                                                -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(fh_options INTERFACE -fsanitize=${FH_SANITIZE})
  endif()
endif()

add_library(feedhandler
  src/cpu.cpp
  src/fast_book.cpp
  src/mapped_file.cpp
  src/os_memory.cpp
  src/reference_book.cpp
  src/summary.cpp
)
target_include_directories(feedhandler PUBLIC include)
target_link_libraries(feedhandler PUBLIC fh_options Threads::Threads)

function(fh_executable name)
  add_executable(${name} ${ARGN} src/alloc_counter.cpp)
  target_include_directories(${name} PRIVATE tools)
  target_link_libraries(${name} PRIVATE feedhandler)
endfunction()

fh_executable(itch_bench tools/bench.cpp)
fh_executable(itch_verify tools/verify.cpp)
fh_executable(itch_pipeline tools/pipeline.cpp)
fh_executable(itch_gen tools/gen.cpp)

if(FH_BUILD_TESTS)
  enable_testing()
  fh_executable(fh_tests tests/tests.cpp)
  add_test(NAME unit COMMAND fh_tests)

  set(sample ${CMAKE_CURRENT_BINARY_DIR}/sample.itch)
  add_test(NAME generate_sample
           COMMAND itch_gen ${sample} --messages 2000000 --symbols 64 --seed 7)
  set_tests_properties(generate_sample PROPERTIES FIXTURES_SETUP sample)
  add_test(NAME verify_sample COMMAND itch_verify ${sample})
  add_test(NAME bench_sample COMMAND itch_bench ${sample})
  add_test(NAME pipeline_sample COMMAND itch_pipeline ${sample})
  set_tests_properties(verify_sample bench_sample pipeline_sample
                       PROPERTIES FIXTURES_REQUIRED sample)
endif()
