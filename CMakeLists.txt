cmake_minimum_required(VERSION 3.16)

project(video_capture_libretro VERSION 0.4.5 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Link the MSVC runtime statically so the DLL has no redistributable dependency.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CORE_SOURCES
  src/video_capture_libretro.c
  src/convert.c
  src/font.c
  src/scale.c
)

if(WIN32)
  # Version information resource (file Properties > Details).
  enable_language(RC)
  configure_file(src/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/version.rc @ONLY)
  list(APPEND CORE_SOURCES src/capture_mf.cpp ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
else()
  # No native capture backend yet: the core shows its test pattern only.
  list(APPEND CORE_SOURCES src/capture_null.c)
endif()

add_library(video_capture_libretro SHARED ${CORE_SOURCES})

target_include_directories(video_capture_libretro PRIVATE include src)
target_compile_definitions(video_capture_libretro PRIVATE
  CORE_VERSION="${PROJECT_VERSION}"
)

if(WIN32)
  # Windows 10 APIs (Media Foundation advanced video processing).
  target_compile_definitions(video_capture_libretro PRIVATE
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
    UNICODE
    _UNICODE
  )
  target_link_libraries(video_capture_libretro PRIVATE
    mf mfplat mfreadwrite mfuuid ole32
  )
endif()

# libretro cores are named <name>_libretro.<ext>, without a "lib" prefix.
set_target_properties(video_capture_libretro PROPERTIES
  PREFIX ""
  C_VISIBILITY_PRESET hidden
  CXX_VISIBILITY_PRESET hidden
)

if(MSVC)
  target_compile_options(video_capture_libretro PRIVATE /W4 /utf-8)
else()
  target_compile_options(video_capture_libretro PRIVATE -Wall -Wextra)
endif()
