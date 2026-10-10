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
# Launcher UI. Kept isolated from the runtime so Qt does not leak into Xenon
# core, CPU, memory, graphics, or game-module interfaces.
if(XENON_BUILD_LAUNCHER)
  add_subdirectory(launcher)
endif()

# RuntimeBridge locates xenon_runtime_host next to the launcher executable at
# runtime (see docs/runtime/RUNTIME_HOST.md). Pin the host's output directory to the
# launcher's own so this holds for a plain dev build, not only after
# `cmake --install` places both under the same bin/. xenon_launcher's own
# output directory is left untouched (scripts such as
# launcher/scripts/build-launcher.ps1 expect it at build/<preset>/launcher/).
if(TARGET xenon_runtime_host AND TARGET xenon_launcher)
  set_target_properties(xenon_runtime_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:xenon_launcher>")
  # A launcher-only build still has to be a usable launcher. RuntimeBridge
  # discovers the host beside xenon_launcher, so make it an explicit runtime
  # companion rather than relying on an all-target build to happen first.
  add_dependencies(xenon_launcher xenon_runtime_host)
endif()

# PreparationService follows the same side-by-side contract for xenon-prepare.
# Without this dependency a successful `--target xenon_launcher` build can
# render the UI but cannot prepare a title on first Play.
if(TARGET xenon-prepare AND TARGET xenon_launcher AND
   XENON_ENABLE_MEMORY AND XENON_ENABLE_KERNEL)
  set_target_properties(xenon-prepare PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:xenon_launcher>")
  add_dependencies(xenon_launcher xenon-prepare)
elseif(TARGET xenon_launcher AND NOT XENON_BUILD_RUNTIME_HOST)
  message(STATUS
    "Xenon preparation companion disabled for the launcher UI-only target")
endif()
# RuntimeBridge tests (xenon_runtime_host_fixture, xenon_runtime_bridge_tests)
# are declared inside launcher/CMakeLists.txt itself, in the same directory
# scope find_package(Qt6 ...) ran in - Qt6::Core etc. are imported targets
# only visible within that subdirectory's scope tree, not back up here.

# -----------------------------------------------------------------------------
# Production installer/package generation. Kept last so every installable
# target and dependency runtime has been resolved before CPack is configured.
if(XENON_BUILD_INSTALLER)
  if(NOT TARGET xenon_launcher)
    message(FATAL_ERROR
      "XENON_BUILD_INSTALLER requires Qt 6.6+ and a built xenon_launcher target")
  endif()
  include(Packaging)
endif()
