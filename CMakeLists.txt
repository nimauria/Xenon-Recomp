cmake_minimum_required(VERSION 3.25)

project(
  XenonRecomp
  VERSION 0.1.0
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(Platforms)
include(Architectures)
include(Warnings)
include(Dependencies)

include(XenonOptions)

# Absolute root of this source tree. Subdirectories use it for paths
# outside their own directory; it stays correct when a generated game
# project adds this tree with add_subdirectory().
set(XENON_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

add_subdirectory(src)
add_subdirectory(tools)

# -----------------------------------------------------------------------------
# Generic runtime/game-host process. The launcher spawns this to actually run
# a game; it never executes guest code itself. See runtime_host/CMakeLists.txt
# and docs/runtime/RUNTIME_HOST.md.
if(XENON_BUILD_RUNTIME_HOST)
  if(NOT XENON_ENABLE_MEMORY OR NOT XENON_ENABLE_FILESYSTEM OR
     NOT XENON_ENABLE_KERNEL OR NOT XENON_ENABLE_INPUT)
    message(FATAL_ERROR
      "XENON_BUILD_RUNTIME_HOST requires Memory, Filesystem, Kernel and Input. "
      "Disable XENON_BUILD_RUNTIME_HOST for a launcher UI-only build.")
  endif()
  add_subdirectory(runtime_host)
elseif(XENON_BUILD_LAUNCHER)
  message(STATUS
    "Xenon runtime host disabled: launcher will build in UI-only mode and report the companion as unavailable")
endif()

if(XENON_BUILD_TESTS)
  enable_testing()
endif()
if(XENON_BUILD_TESTS OR XENON_BUILD_BENCHMARKS)
  add_subdirectory(tests)
endif()

# -----------------------------------------------------------------------------
# Qt-free desktop launcher. The standalone build under launcher-next remains
# the preferred UI workflow; runtime integration is a separate adapter milestone.
if(XENON_BUILD_LAUNCHER)
  set(XENON_NEXT_BUILD_DESKTOP ON CACHE BOOL "Build the CEF desktop launcher" FORCE)
  add_subdirectory(launcher-next)
endif()

# -----------------------------------------------------------------------------
# Production installer/package generation. Kept last so every installable
# target and dependency runtime has been resolved before CPack is configured.
if(XENON_BUILD_INSTALLER)
  message(FATAL_ERROR "The legacy installer is retired. Package the standalone launcher with npm run desktop:package in launcher-next.")
endif()
