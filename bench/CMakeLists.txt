# glint_bench — headless reflow benchmark and layout-snapshot gate.
#
# Standalone project; it pulls glint in the same way the demo does:
#
#   cmake -S third_party/glint/bench -B build/bench -G "Visual Studio 17 2022" -A x64
#   cmake --build build/bench --config Release --target glint_bench
#   build/bench/Release/glint_bench.exe --list
#
# Benchmark Release only: glint compiles RelWithDebInfo with /Od.

cmake_minimum_required(VERSION 3.25)
project(glint_bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(GLINT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(GLINT_REPO_ROOT "${GLINT_DIR}/../.." ABSOLUTE)
if(NOT DEFINED GLINT_DEPS_DIR OR
   NOT EXISTS "${GLINT_DEPS_DIR}/src/skia/include/core/SkCanvas.h")
  set(GLINT_DEPS_DIR "${GLINT_REPO_ROOT}/third_party/skia" CACHE PATH
    "Skia dependency root." FORCE)
endif()

add_subdirectory("${GLINT_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/glint")

# Match Skia's static runtime: /MTd for Debug, /MT otherwise.
if(WIN32)
  foreach(_t glint_core glint_host glint_host_win32 glint_host_win32_support)
    if(TARGET ${_t})
      set_property(TARGET ${_t} PROPERTY INTERFACE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
  endforeach()
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(glint_bench glint_bench.cpp glint_snapshot.hpp)
set_property(TARGET glint_bench PROPERTY
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Counters must be on in every TU that compiles glint headers (ODR), so set
# them on the core interface rather than just this target.
target_compile_definitions(glint_core INTERFACE GLINT_PERF_COUNTERS)
target_include_directories(glint_bench PRIVATE "${CMAKE_CURRENT_LIST_DIR}")

if(WIN32)
  target_link_libraries(glint_bench PRIVATE glint::host_win32)
elseif(APPLE)
  target_link_libraries(glint_bench PRIVATE glint::host_mac)
else()
  target_link_libraries(glint_bench PRIVATE glint::host_linux)
endif()
