cmake_minimum_required(VERSION 3.28)

project(microbrowser VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
enable_testing()

option(MICROBROWSER_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(MICROBROWSER_FUZZ "Enable libFuzzer targets" OFF)
option(MICROBROWSER_PERF_HARNESS_BUILD "Enable perf harness instrumentation and targets" OFF)
set(MICROBROWSER_TEST_SHARDS "24" CACHE STRING
    "Number of parallel ctest shards the test suite is split across")

# --- Build-iteration acceleration -------------------------------------------
# Each of these is a no-op when the underlying tool is absent, so a stock
# toolchain still configures cleanly. Ported from microide, where they are the
# difference between a tolerable and an intolerable inner loop.

option(MICROBROWSER_USE_CCACHE "Use ccache as the compiler launcher when available" ON)
if(MICROBROWSER_USE_CCACHE AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
  find_program(MICROBROWSER_CCACHE ccache)
  if(MICROBROWSER_CCACHE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${MICROBROWSER_CCACHE}")
    set(CMAKE_C_COMPILER_LAUNCHER "${MICROBROWSER_CCACHE}")
    message(STATUS "ccache enabled: ${MICROBROWSER_CCACHE}")
  endif()
endif()

# lld links several times faster than GNU ld, and the test binary relinks on
# every edit. Skipped under LTO, which drives its own plugin/link path.
option(MICROBROWSER_USE_LLD "Use ld.lld for faster links when available (non-LTO builds)" ON)
if(MICROBROWSER_USE_LLD AND NOT CMAKE_INTERPROCEDURAL_OPTIMIZATION
   AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  find_program(MICROBROWSER_LLD ld.lld)
  if(MICROBROWSER_LLD)
    add_link_options(-fuse-ld=lld)
    message(STATUS "Linker: ld.lld (${MICROBROWSER_LLD})")
  endif()
endif()

# Debug info into .dwo files instead of the object/link stream: smaller objects,
# faster links, fully debuggable (gdb/lldb read .dwo transparently).
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options($<$<CONFIG:Debug>:-gsplit-dwarf>)
endif()

if(CMAKE_INTERPROCEDURAL_OPTIMIZATION)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT _mb_ipo_supported OUTPUT _mb_ipo_output)
  if(NOT _mb_ipo_supported)
    message(WARNING "LTO requested but unsupported by this toolchain; disabling.\n${_mb_ipo_output}")
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)
  else()
    message(STATUS "Interprocedural optimization (LTO) enabled.")
  endif()
endif()
# ----------------------------------------------------------------------------

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/microbrowser")

# --- Dependencies ------------------------------------------------------------
# The sanctioned list lives in docs/adr/0001-third-party-dependencies.md. Adding
# to it requires an ADR, not just a find_package line.

find_package(PkgConfig QUIET)
find_package(SDL3 CONFIG QUIET)

set(MICROBROWSER_SDL3_TARGET "")
if(TARGET SDL3::SDL3)
  set(MICROBROWSER_SDL3_TARGET SDL3::SDL3)
elseif(TARGET SDL3::SDL3-shared)
  set(MICROBROWSER_SDL3_TARGET SDL3::SDL3-shared)
elseif(TARGET SDL3::SDL3-static)
  set(MICROBROWSER_SDL3_TARGET SDL3::SDL3-static)
elseif(PkgConfig_FOUND)
  pkg_check_modules(SDL3 QUIET IMPORTED_TARGET sdl3)
  if(TARGET PkgConfig::SDL3)
    set(MICROBROWSER_SDL3_TARGET PkgConfig::SDL3)
  endif()
endif()

if(MICROBROWSER_SDL3_TARGET STREQUAL "")
  message(FATAL_ERROR
    "SDL3 was not found. Install SDL3 development files and rerun CMake.\n"
    "Expected either an SDL3 CMake package or a pkg-config entry named 'sdl3'.")
endif()

# --- Shared compile settings -------------------------------------------------

add_library(microbrowser_settings INTERFACE)
target_include_directories(microbrowser_settings INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(microbrowser_settings INTERFACE cxx_std_20)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(microbrowser_settings INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow                 # a shadowed name in a rasterizer inner loop is a silent wrong-pixel bug
    -Wconversion             # implicit narrowing between pixel/layout coordinate types
    -Wsign-conversion
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wdouble-promotion       # a stray double in a float hot path costs real time
    -Wformat=2)
  if(MICROBROWSER_WARNINGS_AS_ERRORS)
    target_compile_options(microbrowser_settings INTERFACE -Werror)
  endif()
endif()

target_compile_definitions(microbrowser_settings INTERFACE
  $<$<BOOL:${MICROBROWSER_PERF_HARNESS_BUILD}>:MICROBROWSER_PERF_HARNESS_BUILD=1>)

# --- Modules -----------------------------------------------------------------
# One static library per module, with dependencies declared explicitly. The
# authoritative dependency contract is each module's src/<module>/MODULE.deps,
# enforced by the ArchitectureInvariants lint; the link edges below mirror it so
# the build graph tells the same story as the manifests, and so modules compile
# in parallel. See guidelines/architecture.md.

function(microbrowser_module name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;EXTERN" ${ARGN})
  add_library(microbrowser_${name} STATIC ${ARG_SOURCES})
  target_link_libraries(microbrowser_${name} PUBLIC microbrowser_settings ${ARG_DEPS} ${ARG_EXTERN})
  set_target_properties(microbrowser_${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

microbrowser_module(util
  SOURCES
    src/util/Parse.cpp
    src/util/PerformanceCounters.cpp
    src/util/PerformanceTrace.cpp
    src/util/StartupTrace.cpp
    src/util/TraceChannel.cpp)

microbrowser_module(gfx
  SOURCES
    src/gfx/Canvas.cpp
    src/gfx/DirtyRegion.cpp
    src/gfx/DisplayList.cpp
    src/gfx/Geometry.cpp
  DEPS microbrowser_util)

microbrowser_module(ipc
  SOURCES
    src/ipc/InProcessTransport.cpp
    src/ipc/Message.cpp
  DEPS microbrowser_util microbrowser_gfx)

microbrowser_module(engine
  SOURCES
    src/engine/Engine.cpp
  DEPS microbrowser_util microbrowser_gfx microbrowser_ipc)

microbrowser_module(platform
  SOURCES
    src/platform/AppDirectories.cpp
    src/platform/SdlPresenter.cpp
    src/platform/SdlWindow.cpp
  DEPS microbrowser_util microbrowser_gfx microbrowser_ipc
  EXTERN ${MICROBROWSER_SDL3_TARGET})

microbrowser_module(app
  SOURCES
    src/app/Application.cpp
    src/app/AppStartupOptions.cpp
    src/app/DirtyRegionPolicy.cpp
  DEPS microbrowser_util microbrowser_gfx microbrowser_ipc
       microbrowser_engine microbrowser_platform)

# --- Binaries ----------------------------------------------------------------

add_executable(microbrowser src/app/main.cpp)
target_link_libraries(microbrowser PRIVATE microbrowser_app)

add_executable(microbrowser_tests
  tests/TestMain.cpp
  tests/TestSupport.cpp
  tests/ArchitectureInvariantsTests.cpp
  tests/architecture/ModuleManifest.cpp
  tests/architecture/SourceScan.cpp
  tests/CanvasTests.cpp
  tests/DirtyRegionPolicyTests.cpp
  tests/DirtyRegionTests.cpp
  tests/DisplayListTests.cpp
  tests/GeometryTests.cpp
  tests/IdleWaitStrategyTests.cpp
  tests/IpcMessageTests.cpp
  tests/ReferenceImageTests.cpp
  tests/support/ReferenceImage.cpp)
target_link_libraries(microbrowser_tests PRIVATE microbrowser_app)
target_compile_definitions(microbrowser_tests PRIVATE
  MICROBROWSER_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")

# The suite is registered as N ctest tests, each running a deterministic
# round-robin slice, so `ctest -jN` uses every core instead of running the whole
# suite in one serial process. This is the dominant win under the sanitizers.
# Set -DMICROBROWSER_TEST_SHARDS=1 to get a single test back.
if(MICROBROWSER_TEST_SHARDS GREATER 1)
  math(EXPR _mb_last_shard "${MICROBROWSER_TEST_SHARDS} - 1")
  foreach(shard RANGE ${_mb_last_shard})
    add_test(NAME microbrowser_tests_${shard}
             COMMAND microbrowser_tests
                     --shard-index=${shard}
                     --shard-count=${MICROBROWSER_TEST_SHARDS})
  endforeach()
else()
  add_test(NAME microbrowser_tests COMMAND microbrowser_tests)
endif()
