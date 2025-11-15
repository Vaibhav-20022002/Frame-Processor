# CMake version required to build this project
cmake_minimum_required(VERSION 3.16)

# Declares the project name and the language (CXX for C++)
project(frame_processor LANGUAGES CXX)

# Set the C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to a Release build if no type is specified
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Set custom compiler options for custom build upon DEBUG build type
set(DEBUG_COMPILE_FLAGS
  -g
  -O0
  -pipe
)

# Set custom compiler options for custom build upon RELEASE build type
set(RELEASE_COMPILE_FLAGS
  -O2
  -pipe
  -flto
)

# Find all required dependencies using modern CMake targets
find_package(PkgConfig REQUIRED)
find_package(fmt 11.0 REQUIRED)
find_package(simdjson 3.11 REQUIRED)

pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(AVFILTER REQUIRED IMPORTED_TARGET libavfilter)
pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)

# Define the executable target and its source files
add_executable(frame_processor
  src/core/main.cpp
  src/components/config_manager/config_manager.cpp
  src/components/stream_io/stream_io_manager.cpp
  src/components/worker/worker.cpp
  src/components/publisher/publisher.cpp
  src/utils/logger.cpp
  src/utils/types.cpp
)

# This properly scopes the include path to only this target.
target_include_directories(frame_processor PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Add compile options based on build type
target_compile_options(frame_processor PRIVATE
  $<$<CONFIG:Debug>:${DEBUG_COMPILE_FLAGS}>
  $<$<CONFIG:Release>:${RELEASE_COMPILE_FLAGS}>
)

# If using Clang and LTO, force the use of lld at link time so bitcode objects are handled.
# Use a target-level directive that only applies for Release builds when using Clang.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_link_options(frame_processor PRIVATE
    $<$<AND:$<CONFIG:Release>,$<CXX_COMPILER_ID:Clang>>:-fuse-ld=lld>
  )
endif()

# Link all the required libraries to the executable
target_link_libraries(frame_processor PRIVATE
  PkgConfig::AVFORMAT
  PkgConfig::AVCODEC
  PkgConfig::AVUTIL
  PkgConfig::SWSCALE
  PkgConfig::AVFILTER
  PkgConfig::LIBCURL
  fmt::fmt
  simdjson::simdjson
  Threads::Threads    # Link pthreads
  PkgConfig::HIREDIS
)
