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
  src/mapped_file.cpp
  src/summary.cpp
)
target_include_directories(feedhandler PUBLIC include)
target_link_libraries(feedhandler PUBLIC fh_options Threads::Threads)
