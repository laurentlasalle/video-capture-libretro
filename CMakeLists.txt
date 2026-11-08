cmake_minimum_required(VERSION 3.16)

project(video_capture_libretro VERSION 0.0.1 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Link the MSVC runtime statically so the DLL has no redistributable dependency.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_library(video_capture_libretro SHARED
  src/video_capture_libretro.c
)

target_include_directories(video_capture_libretro PRIVATE include)
target_compile_definitions(video_capture_libretro PRIVATE
  CORE_VERSION="${PROJECT_VERSION}"
)

# libretro cores are named <name>_libretro.<ext>, without a "lib" prefix.
set_target_properties(video_capture_libretro PROPERTIES
  PREFIX ""
  C_VISIBILITY_PRESET hidden
)

if(MSVC)
  target_compile_options(video_capture_libretro PRIVATE /W4)
else()
  target_compile_options(video_capture_libretro PRIVATE -Wall -Wextra)
endif()
