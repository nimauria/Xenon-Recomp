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

option(XENON_BUILD_TESTS "Build Xenon Recomp tests" ON)
option(XENON_BUILD_BENCHMARKS "Build Xenon Memory V2 benchmarks" ON)
option(XENON_BUILD_LAUNCHER "Build the Xenon launcher UI when Qt 6 is available" ON)
option(XENON_BUILD_INSTALLER "Configure production installer/package generation" OFF)
option(XENON_ENABLE_MEMORY "Build the production Xenon memory subsystem" ON)
set(XENON_MEMORY_DIRECT_APERTURE_QUALIFIED_DEFAULT OFF)
# Phase 6 keeps compact translation as Auto/default. Repeated paired Release
# measurements on the current Linux x86-64 baseline show compact translation
# outperforming the direct aperture. A platform may opt in after qualification.
option(XENON_MEMORY_DEFAULT_DIRECT_APERTURE
  "Use the optional direct guest-address aperture as AddressSpace Auto mode"
  ${XENON_MEMORY_DIRECT_APERTURE_QUALIFIED_DEFAULT})
option(XENON_ENABLE_GRAPHICS "Build the Xenos graphics frontend" ON)
option(XENON_ENABLE_VULKAN "Build the Vulkan backend when the SDK is available" ON)
option(XENON_ENABLE_D3D12 "Build the Direct3D 12 backend on Windows" ON)
option(XENON_ENABLE_DXC "Build the HLSL/DXC shader compiler when DXC is available" ON)
option(XENON_ENABLE_AUDIO "Build the production Xbox audio translation subsystem" ON)
set(XENON_AUDIO_FFMPEG_ROOT "" CACHE PATH
  "Optional FFmpeg root exposing AV_CODEC_ID_XMAFRAMES; overrides Xenon managed/bundled discovery")
option(XENON_ENABLE_INPUT "Enable input subsystem" ON)
option(XENON_INPUT_ENABLE_SDL2 "Enable SDL2 controller backend when SDL2 is available" ON)
option(XENON_INPUT_ENABLE_SDL3 "Enable SDL3 controller backend when SDL3 is available" ON)
option(XENON_INPUT_ENABLE_XINPUT "Enable native Windows XInput backend" ON)
option(XENON_ENABLE_NETWORK "Enable networking subsystem" ON)
option(XENON_ENABLE_FILESYSTEM "Build the Xenon virtual filesystem subsystem" ON)
option(XENON_ENABLE_KERNEL "Build the Xenon host-side kernel object/I/O layer" ${XENON_ENABLE_FILESYSTEM})

# A UI-only configuration deliberately disables the subsystems XenonSession
# requires. Do not still force its process host into that build: static-library
# link failures there previously made an otherwise independent launcher target
# impossible to build. Full/default configurations retain the companion.
set(_xenon_runtime_host_default ON)
if(NOT XENON_ENABLE_MEMORY OR NOT XENON_ENABLE_FILESYSTEM OR
   NOT XENON_ENABLE_KERNEL OR NOT XENON_ENABLE_INPUT)
  set(_xenon_runtime_host_default OFF)
endif()
option(XENON_BUILD_RUNTIME_HOST
  "Build the guest-executing xenon_runtime_host companion"
  ${_xenon_runtime_host_default})
unset(_xenon_runtime_host_default)

if(XENON_ENABLE_GRAPHICS AND NOT XENON_ENABLE_MEMORY)
  message(FATAL_ERROR "XENON_ENABLE_GRAPHICS requires XENON_ENABLE_MEMORY")
endif()
if(XENON_ENABLE_KERNEL AND NOT XENON_ENABLE_FILESYSTEM)
  message(FATAL_ERROR "XENON_ENABLE_KERNEL currently requires XENON_ENABLE_FILESYSTEM")
endif()
if(XENON_ENABLE_AUDIO AND NOT XENON_ENABLE_MEMORY)
  message(FATAL_ERROR "XENON_ENABLE_AUDIO requires XENON_ENABLE_MEMORY")
endif()

# -----------------------------------------------------------------------------
# Core runtime
add_library(xenon_core STATIC
  src/core/runtime.cpp
  src/core/session.cpp
  src/core/guest_thread_context.cpp
  src/core/export_registry.cpp
  src/core/call_bridge.cpp
  src/core/run_fingerprint.cpp
  src/core/capability_report.cpp
  src/core/import_classification.cpp
  src/logging/logger.cpp
  src/xam/user_manager.cpp
  src/xam/locale_manager.cpp
  src/xam/content_manager.cpp
  src/xam/content_graph.cpp
  src/xam/title_update_manager.cpp
  src/xam/dlc_manager.cpp
  src/xam/save_manager.cpp
  src/xam/notification_manager.cpp
  src/xam/achievement_manager.cpp
  src/xam/xam_session.cpp
  src/xam/xam_user_exports.cpp
  src/xam/xam_locale_exports.cpp
  src/xam/xam_content_exports.cpp
  src/xam/xam_notification_exports.cpp
  src/xam/xam_achievement_exports.cpp
)
target_include_directories(xenon_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(xenon_core PUBLIC cxx_std_20)
xenon_enable_warnings(xenon_core)
# Session depends on all major subsystems, but none of those subsystem
# targets (xenon_memory, xenon_cpu, xenon_filesystem, xenon_kernel,
# xenon_input, xenon_graphics, xenon_xbox_kernel_io) exist yet at this point
# in the file - add_library() for each of them runs further down. The actual
# `if(TARGET ...)`-guarded target_link_libraries(xenon_core ...) calls live
# after "Core runtime dependencies" below (right before the Tests section),
# once every optional subsystem has had a chance to be defined; putting them
# here would make every `if(TARGET ...)` check evaluate false and silently
# leave xenon_core without any of its real dependencies.
add_library(Xenon::Core ALIAS xenon_core)

# -----------------------------------------------------------------------------
# Xenon Network client/uplink. The protocol and service boundary are kept in a
# Qt-free, platform-neutral target. Concrete HTTPS/realtime transports live at
# the host edge (the launcher supplies the first Qt implementation) so Xbox/XAM
# compatibility code never sees URLs, JSON, or a particular socket library.
if(XENON_ENABLE_NETWORK)
  add_library(xenon_network STATIC
    src/network/types.cpp
    src/network/routes.cpp
    src/network/client.cpp
    src/network/realtime.cpp
    src/network/xbox_services.cpp
  )
  target_include_directories(xenon_network PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_compile_features(xenon_network PUBLIC cxx_std_20)
  target_link_libraries(xenon_network PUBLIC Xenon::Core)
  xenon_enable_warnings(xenon_network)
  add_library(Xenon::Network ALIAS xenon_network)
endif()

# -----------------------------------------------------------------------------
# Filesystem: host-independent Xbox-style VFS and content providers.
# This target deliberately remains independent of guest Memory.
if(XENON_ENABLE_FILESYSTEM)
  add_library(xenon_filesystem STATIC
    src/filesystem/content/xex_metadata.cpp
    src/filesystem/content/content_probe.cpp
    src/filesystem/content/gdfx_image_source.cpp
    src/filesystem/content/stfs_package.cpp
    src/filesystem/content/stfs_package_source.cpp
    src/filesystem/content/content_materializer.cpp
    src/filesystem/virtual/path.cpp
    src/filesystem/virtual/device.cpp
    src/filesystem/virtual/directory_cursor.cpp
    src/filesystem/virtual/virtual_file_system.cpp
    src/filesystem/devices/null_device.cpp
    src/filesystem/devices/read_only_content_device.cpp
    src/filesystem/host/host_path_device.cpp
  )
  target_include_directories(xenon_filesystem PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_compile_features(xenon_filesystem PUBLIC cxx_std_20)
  xenon_enable_warnings(xenon_filesystem)
  add_library(Xenon::Filesystem ALIAS xenon_filesystem)
endif()

# -----------------------------------------------------------------------------
# Host-side kernel object/I/O layer. This still has no guest Memory dependency.
if(XENON_ENABLE_KERNEL)
  add_library(xenon_kernel STATIC
    src/kernel/handles/object.cpp
    src/kernel/handles/handle_table.cpp
    src/kernel/handles/io_request.cpp
    src/kernel/handles/file_object.cpp
    src/kernel/handles/io_manager.cpp
    src/kernel/xboxkrnl/xbox_io.cpp
    src/kernel/synchronization/completion_port.cpp
    src/kernel/synchronization/event.cpp
    src/kernel/synchronization/semaphore.cpp
    src/kernel/synchronization/mutant.cpp
    src/kernel/synchronization/timer.cpp
    src/kernel/synchronization/timer_manager.cpp
    src/kernel/synchronization/wait.cpp
    src/kernel/threading/thread.cpp
    src/kernel/timing/time.cpp
    src/kernel/module/module.cpp
    src/kernel/exception/exception.cpp
  )
  target_link_libraries(xenon_kernel PUBLIC xenon_filesystem)
  target_include_directories(xenon_kernel PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_compile_features(xenon_kernel PUBLIC cxx_std_20)
  xenon_enable_warnings(xenon_kernel)
  add_library(Xenon::Kernel ALIAS xenon_kernel)

  # Kernel memory integration requires Memory V2
  if(XENON_ENABLE_MEMORY)
    target_sources(xenon_kernel PRIVATE
      src/kernel/memory/memory.cpp
      src/kernel/memory/heap.cpp
      src/kernel/process/process.cpp
    )
    target_link_libraries(xenon_kernel PUBLIC xenon_memory)
  endif()
endif()

# -----------------------------------------------------------------------------
# CPU: Xenon PPC/VMX128 frontend -> Xenon IR -> optimizer -> native C++ AOT.
add_library(xenon_cpu STATIC
  src/cpu/ppc/state.cpp
  src/cpu/external_calls.cpp
  src/cpu/ppc/decoder.cpp
  src/cpu/ppc/lifter.cpp
  src/cpu/ppc/lifter_integer.cpp
  src/cpu/ppc/lifter_control.cpp
  src/cpu/ppc/lifter_memory.cpp
  src/cpu/ppc/lifter_fpu.cpp
  src/cpu/ppc/lifter_vector.cpp
  src/cpu/ppc/flat_memory.cpp
  src/cpu/ir/ir.cpp
  src/cpu/ir/verifier.cpp
  src/cpu/optimizer/optimizer.cpp
  src/cpu/codegen/aot_semantics.cpp
  src/cpu/codegen/backend_cpp_aot.cpp
  src/cpu/codegen/dynamic_fallback.cpp
  src/cpu/codegen/executable_code_cache.cpp
  src/cpu/codegen/function_compiler.cpp
  src/cpu/verification/semantic_verifier.cpp
)
target_include_directories(xenon_cpu
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/ppc"
)
target_compile_features(xenon_cpu PUBLIC cxx_std_20)
xenon_enable_guest_fp_semantics(xenon_cpu PUBLIC)
xenon_enable_warnings(xenon_cpu)
if(MSVC)
  # decoder.cpp builds its opcode dispatch table via a consteval bucket-sort
  # over the full PPC opcode catalog; this legitimately exceeds MSVC's
  # default constexpr evaluation step budget (1,048,576).
  target_compile_options(xenon_cpu PRIVATE /constexpr:steps100000000)
endif()
add_library(Xenon::CPU ALIAS xenon_cpu)

# Whole-game static recompilation driver and its diagnostic utilities.
# Gen 10 producer identities follow implementation changes even if a developer
# forgets a manual ABI bump. CMake reconfigures when any producer input changes.
file(GLOB_RECURSE _graph_ir_inputs CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/ppc/*"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/ir/*"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/optimizer/*"
  "${CMAKE_CURRENT_SOURCE_DIR}/include/xenon/cpu/*.hpp")
list(APPEND _graph_ir_inputs "${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/codegen/function_compiler.cpp")
set(_graph_ir_identity "")
foreach(_input IN LISTS _graph_ir_inputs)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_input}")
  file(SHA256 "${_input}" _hash)
  string(APPEND _graph_ir_identity "${_hash};")
endforeach()
string(SHA256 XENON_GRAPH_IR_PRODUCER "${_graph_ir_identity}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/codegen/backend_cpp_aot.cpp")
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/codegen/backend_cpp_aot.cpp" XENON_GRAPH_CODEGEN_PRODUCER)
find_package(Threads REQUIRED)

add_library(xenon_recomp STATIC
  src/recomp/driver.cpp
  src/recomp/ppc_value_analysis.cpp
  src/recomp/knowledge_base.cpp
  src/recomp/compilation_graph.cpp
  src/recomp/game_intake.cpp
  src/recomp/analysis_schema.cpp
  src/recomp/analysis_schema_json.cpp
  src/recomp/native_replacements.cpp
  src/recomp/runtime_helpers.cpp
  src/recomp/module_hint_provider.cpp
  src/recomp/artifact_cache.cpp
  src/recomp/worker_pool.cpp
)
target_compile_definitions(xenon_recomp PRIVATE
  XENON_GRAPH_IR_PRODUCER="${XENON_GRAPH_IR_PRODUCER}"
  XENON_GRAPH_CODEGEN_PRODUCER="${XENON_GRAPH_CODEGEN_PRODUCER}")

target_include_directories(xenon_recomp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(xenon_recomp PUBLIC xenon_cpu xenon_xbox_kernel_io Threads::Threads)
target_compile_features(xenon_recomp PUBLIC cxx_std_20)
xenon_enable_warnings(xenon_recomp)
add_library(Xenon::Recomp ALIAS xenon_recomp)

foreach(xenon_tool recomp-driver ppc-disasm ir-dump module-inspector)
  add_executable(${xenon_tool} tools/recomp_tools.cpp)
  target_compile_definitions(${xenon_tool} PRIVATE XENON_TOOL_NAME="${xenon_tool}")
  target_link_libraries(${xenon_tool} PRIVATE xenon_recomp)
  xenon_enable_warnings(${xenon_tool})
endforeach()

# import-scanner additionally links xenon_core: for Project Gracemeria import
# auditing (see docs/xam/XAM_V1.md), it cross-references every import against a
# real, minimally-initialized XenonSession export_registry_ - the exact same
# production registry XamSession::register_exports() populates - so its
# "implemented/not implemented" verdicts reflect production resolution, not
# a second, hand-maintained list. Deliberately game-agnostic: it inspects
# whatever XEX is passed on the command line, never a hard-coded title.
add_executable(import-scanner tools/recomp_tools.cpp)
target_compile_definitions(import-scanner PRIVATE XENON_TOOL_NAME="import-scanner" XENON_TOOL_HAS_CORE=1)
target_link_libraries(import-scanner PRIVATE xenon_recomp xenon_core)
xenon_enable_warnings(import-scanner)

# xenon-prepare: the out-of-process automatic game preparation worker (see
# docs/development/GAME_PREPARATION.md). Reads default.xex directly out of a disc image/
# directory (xenon_filesystem, transitively linked via xenon_recomp ->
# xenon_xbox_kernel_io -> xenon_kernel -> xenon_filesystem), analyzes and
# generates native source (xenon_recomp), then drives a nested CMake build
# and promotes the result through the artifact cache. XENON_SOURCE_ROOT lets
# it default --recomp-root to this checkout, matching xenon_recomp_driver_tests.
add_executable(xenon-prepare tools/xenon_prepare.cpp)
target_link_libraries(xenon-prepare PRIVATE xenon_recomp)
target_compile_definitions(xenon-prepare PRIVATE XENON_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
target_compile_definitions(xenon-prepare PRIVATE
  XENON_NATIVE_COMPILER="${CMAKE_CXX_COMPILER}"
  XENON_CMAKE_COMMAND="${CMAKE_COMMAND}")

if(NOT WIN32)
  target_link_libraries(xenon-prepare PRIVATE ${CMAKE_DL_LIBS})
endif()
xenon_enable_warnings(xenon-prepare)

# -----------------------------------------------------------------------------
# Production guest memory. FlatMemory above remains a CPU test fixture only.
if(XENON_ENABLE_MEMORY)
  if(WIN32)
    set(XENON_HOST_VM_SOURCE src/memory/mapping/host_vm_windows.cpp)
  elseif(UNIX)
    set(XENON_HOST_VM_SOURCE src/memory/mapping/host_vm_posix.cpp)
  else()
    set(XENON_HOST_VM_SOURCE src/memory/mapping/host_vm_fallback.cpp)
  endif()
  add_library(xenon_memory STATIC
    src/memory/guest/address_space.cpp
    ${XENON_HOST_VM_SOURCE}
  )
  target_link_libraries(xenon_memory PUBLIC xenon_cpu)
  target_include_directories(xenon_memory PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_compile_features(xenon_memory PUBLIC cxx_std_20)
  target_compile_definitions(xenon_memory PUBLIC
    XENON_MEMORY_DEFAULT_DIRECT_APERTURE=$<BOOL:${XENON_MEMORY_DEFAULT_DIRECT_APERTURE}>)
  xenon_enable_warnings(xenon_memory)
  add_library(Xenon::Memory ALIAS xenon_memory)
endif()

# -----------------------------------------------------------------------------
# Audio: Xbox render-driver/XMA semantics -> bounded Xenon mixer -> SDL2 host.
# FFmpeg supplies the legally redistributable XMAFRAMES decoder API; Xenon does
# not vendor an XMA codec or substitute a silent/null production backend.
if(XENON_ENABLE_AUDIO)
  xenon_resolve_sdl2(XENON_AUDIO_SDL_TARGET)
  xenon_resolve_audio_ffmpeg(
    XENON_AUDIO_FFMPEG_TARGET
    XENON_AUDIO_AVCODEC_INCLUDE_DIR
    XENON_AUDIO_USING_BUNDLED_FFMPEG)

  if(NOT XENON_AUDIO_SDL_TARGET)
    xenon_dependency_help("sdl2")
    message(FATAL_ERROR
      "Audio V1 requires SDL2. Xenon could not resolve or bootstrap the pinned dependency. "
      "Use XENON_DEPENDENCY_MODE=SYSTEM for a host package, set XENON_SDL2_ROOT, "
      "or provision the managed dependency with the command above.")
  endif()
  if(NOT XENON_AUDIO_FFMPEG_TARGET)
    xenon_dependency_help("xenia-ffmpeg")
    message(FATAL_ERROR
      "Audio V1 requires libavcodec/libavutil with AV_CODEC_ID_XMAFRAMES. "
      "Use Xenon's managed dependency bootstrap, the committed Windows x64 developer bundle, "
      "or set XENON_AUDIO_FFMPEG_ROOT to a compatible xenia-project FFmpeg build.")
  endif()

  include(CheckCXXSourceCompiles)
  # Header/API capability probes. Static-library try-compiles avoid attempting
  # to execute target binaries and work with prebuilt Windows archives.
  set(_xenon_audio_saved_try_compile_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
  set(CMAKE_REQUIRED_INCLUDES "${XENON_AUDIO_AVCODEC_INCLUDE_DIR}")
  check_cxx_source_compiles([=[
    extern "C" {
      #include <libavcodec/avcodec.h>
    }
    int main() { auto id = AV_CODEC_ID_XMAFRAMES; return static_cast<int>(id == AV_CODEC_ID_NONE); }
  ]=] XENON_AUDIO_FFMPEG_HAS_XMAFRAMES)
  if(NOT XENON_AUDIO_FFMPEG_HAS_XMAFRAMES)
    message(FATAL_ERROR
      "The selected FFmpeg does not expose AV_CODEC_ID_XMAFRAMES. Mainline FFmpeg is not a substitute for Xenon's raw XMA hardware-frame path. Use the managed dependency bootstrap, the vetted Windows x64 developer bundle, or XENON_AUDIO_FFMPEG_ROOT.")
  endif()

  check_cxx_source_compiles([=[
    extern "C" {
      #include <libavcodec/avcodec.h>
      #include <libavutil/channel_layout.h>
    }
    int main() { AVCodecContext* c = nullptr; av_channel_layout_default(&c->ch_layout, 2); return 0; }
  ]=] XENON_AUDIO_FFMPEG_HAS_CH_LAYOUT)
  if(_xenon_audio_saved_try_compile_target_type)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_xenon_audio_saved_try_compile_target_type}")
  else()
    unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
  endif()
  unset(CMAKE_REQUIRED_INCLUDES)
  unset(_xenon_audio_saved_try_compile_target_type)

  add_library(xenon_audio STATIC
    src/audio/backend_sdl.cpp
    src/audio/mixer.cpp
    src/audio/system.cpp
    src/audio/xma.cpp
  )
  target_include_directories(xenon_audio PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(xenon_audio
    PUBLIC xenon_memory
    PRIVATE ${XENON_AUDIO_SDL_TARGET} ${XENON_AUDIO_FFMPEG_TARGET})
  target_compile_features(xenon_audio PUBLIC cxx_std_20)
  if(XENON_AUDIO_FFMPEG_HAS_CH_LAYOUT)
    target_compile_definitions(xenon_audio PRIVATE XENON_AUDIO_FFMPEG_HAS_CH_LAYOUT=1)
  endif()
  if(XENON_AUDIO_USING_BUNDLED_FFMPEG)
    target_compile_definitions(xenon_audio PRIVATE XENON_AUDIO_BUNDLED_FFMPEG=1)
  endif()
  xenon_enable_warnings(xenon_audio)
  add_library(Xenon::Audio ALIAS xenon_audio)

  target_sources(xenon_core PRIVATE src/audio/exports.cpp)
  target_compile_definitions(xenon_core PUBLIC XENON_HAS_AUDIO=1)
endif()

# -----------------------------------------------------------------------------
# Input: host-neutral Xbox controller model. Platform backends are optional.
add_library(xenon_input_api INTERFACE)
target_include_directories(xenon_input_api INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(xenon_input_api INTERFACE cxx_std_20)
add_library(Xenon::InputAPI ALIAS xenon_input_api)

if(XENON_ENABLE_INPUT)
  add_library(xenon_input STATIC
    src/input/action_router.cpp
    src/input/system.cpp
    src/input/module_api.cpp
    src/input/state_merge.cpp
    src/input/profile.cpp
    src/input/flight_driver.cpp
    src/input/xinput/xinput_driver.cpp
    src/input/keyboard_mouse_driver.cpp
    src/input/xam_facade.cpp
    src/input/null_driver.cpp
    src/input/sdl/sdl_driver.cpp
  )
  target_include_directories(xenon_input PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(xenon_input PUBLIC xenon_input_api)
  target_compile_features(xenon_input PUBLIC cxx_std_20)
  xenon_enable_warnings(xenon_input)
  add_library(Xenon::Input ALIAS xenon_input)

  if(WIN32 AND XENON_INPUT_ENABLE_XINPUT)
    target_sources(xenon_input PRIVATE src/input/xinput/xinput_host_win32.cpp)
    target_compile_definitions(xenon_input PUBLIC XENON_INPUT_HAS_XINPUT=1)
  endif()

  if(XENON_INPUT_ENABLE_SDL3)
    find_package(SDL3 3.2 QUIET CONFIG)
    if(TARGET SDL3::SDL3)
      target_sources(xenon_input PRIVATE src/input/sdl/sdl3_host.cpp)
      target_link_libraries(xenon_input PRIVATE SDL3::SDL3)
      target_compile_definitions(xenon_input PUBLIC XENON_INPUT_HAS_SDL3=1)
    endif()
  endif()

  if(XENON_INPUT_ENABLE_SDL2)
    xenon_resolve_sdl2(XENON_SDL2_TARGET)
    if(XENON_SDL2_TARGET)
      target_sources(xenon_input PRIVATE src/input/sdl/sdl2_host.cpp)
      target_link_libraries(xenon_input PRIVATE ${XENON_SDL2_TARGET})
      target_compile_definitions(xenon_input PUBLIC XENON_INPUT_HAS_SDL2=1)
    endif()
  endif()

  if(XENON_ENABLE_MEMORY)
    add_library(xenon_input_guest STATIC src/input/xam_guest.cpp)
    target_include_directories(xenon_input_guest PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(xenon_input_guest PUBLIC xenon_input xenon_memory)
    target_compile_features(xenon_input_guest PUBLIC cxx_std_20)
    xenon_enable_warnings(xenon_input_guest)
    add_library(Xenon::InputGuest ALIAS xenon_input_guest)
  endif()
endif()

# Guest xboxkrnl filesystem ABI bridge. Kept separate so Xenon::Kernel can
# still be built/tested without guest memory.
if(XENON_ENABLE_KERNEL AND XENON_ENABLE_MEMORY)
  add_library(xenon_xbox_kernel_io STATIC
    src/kernel/xboxkrnl/xbox_io_guest.cpp
    src/xbox/imports/import_registry.cpp
    src/xbox/exports/xboxkrnl_io_exports.cpp
    src/xbox/exports/xboxkrnl_rtl_exports.cpp
    src/xbox/exports/xboxkrnl_time_exports.cpp
    src/xbox/exports/xboxkrnl_sync_exports.cpp
    src/xbox/xbox_time_convert.cpp
    src/xbox/rtl.cpp
    src/xbox/export_metadata.cpp
    src/xbox/xex/xex_loader.cpp
    src/xbox/xex/xex_crypto.cpp
    src/xbox/xex/xex_lzx.cpp
  )
  target_link_libraries(xenon_xbox_kernel_io PUBLIC xenon_kernel xenon_memory)
  target_include_directories(xenon_xbox_kernel_io PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_compile_features(xenon_xbox_kernel_io PUBLIC cxx_std_20)
  xenon_enable_warnings(xenon_xbox_kernel_io)
  add_library(Xenon::XboxKernelIo ALIAS xenon_xbox_kernel_io)
endif()

if(XENON_BUILD_BENCHMARKS AND XENON_ENABLE_MEMORY)
  add_executable(xenon_memory_v2_benchmarks
    tests/memory/memory_benchmarks.cpp)
  target_link_libraries(xenon_memory_v2_benchmarks PRIVATE xenon_memory)
  target_compile_definitions(xenon_memory_v2_benchmarks PRIVATE
    XENON_BENCHMARK_BUILD_CONFIG="$<CONFIG>")
  xenon_enable_test_asserts(xenon_memory_v2_benchmarks)
endif()

# Recomp Analysis V2 synthetic benchmark (Part 20): jobs=1 vs jobs=auto wall
# time on a large synthetic independent-function workload, no copyrighted
# content. Not registered as a ctest (it prints timing, not pass/fail) -
# run manually, e.g. `xenon_recomp_analysis_benchmark 5000 24`.
if(XENON_BUILD_BENCHMARKS)
  add_executable(xenon_recomp_analysis_benchmark
    tests/recomp/recomp_analysis_benchmark.cpp)
  target_link_libraries(xenon_recomp_analysis_benchmark PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_recomp_analysis_benchmark)
endif()

# -----------------------------------------------------------------------------
# Graphics: Xenos PM4/shader/EDRAM frontend. Vulkan and D3D12 remain backends.
if(XENON_ENABLE_GRAPHICS)
  add_library(xenon_graphics STATIC
    src/graphics/xenos/register_file.cpp
    src/graphics/xenos/shader.cpp
    src/graphics/xenos/shader_decoder.cpp
    src/graphics/xenos/shader_translation.cpp
    src/graphics/xenos/depth_format.cpp
    src/graphics/xenos/resource_ir.cpp
    src/graphics/xenos/resource_barrier.cpp
    src/graphics/xenos/texture.cpp
    src/graphics/xenos/presentation.cpp
    src/graphics/xenos/edram.cpp
    src/graphics/xenos/edram_surface.cpp
    src/graphics/xenos/edram_ownership.cpp
    src/graphics/xenos/primitive_processor.cpp
    src/graphics/xenos/command_processor.cpp
    src/graphics/xenos/capture.cpp
    src/graphics/xenos/graphics_system.cpp
    src/graphics/windows/backend_capabilities.cpp
  )
  target_link_libraries(xenon_graphics PUBLIC xenon_memory)
  if(WIN32)
    # Runtime capability discovery lives in the common layer and calls DXGI
    # and D3D12 even when the optional renderer target is disabled.
    target_link_libraries(xenon_graphics PRIVATE d3d12 dxgi)
  endif()
  target_include_directories(xenon_graphics PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_compile_features(xenon_graphics PUBLIC cxx_std_20)
  xenon_enable_warnings(xenon_graphics)
  add_library(Xenon::Graphics ALIAS xenon_graphics)
  add_library(Xenon::GPU ALIAS xenon_graphics)
  # Compatibility with the phase-1 working tree target name.
  add_library(xenon_gpu ALIAS xenon_graphics)

  if(XENON_ENABLE_DXC)
    xenon_resolve_dxc(XENON_DXC_TARGET XENON_DXC_INCLUDE_DIR)
    if(XENON_DXC_TARGET AND XENON_DXC_INCLUDE_DIR)
      add_library(xenon_graphics_dxc STATIC
        src/graphics/dxc/shader_compiler.cpp)
      target_include_directories(xenon_graphics_dxc
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
        PRIVATE "${XENON_DXC_INCLUDE_DIR}")
      target_link_libraries(xenon_graphics_dxc PUBLIC xenon_graphics
        PRIVATE ${XENON_DXC_TARGET})
      target_compile_features(xenon_graphics_dxc PUBLIC cxx_std_20)
      xenon_enable_warnings(xenon_graphics_dxc)
      add_library(Xenon::GraphicsDXC ALIAS xenon_graphics_dxc)
      message(STATUS "DXC shader compiler enabled through Xenon dependency resolver")
    else()
      message(STATUS "DXC shader compiler disabled: compatible DXC runtime/development files not found")
    endif()
  endif()

  if(XENON_ENABLE_VULKAN)
    xenon_resolve_vulkan(XENON_VULKAN_TARGET)
    if(XENON_VULKAN_TARGET)
      target_compile_definitions(xenon_graphics PRIVATE XENON_HAS_VULKAN_SDK=1)
      add_library(xenon_graphics_vulkan STATIC
        src/graphics/vulkan/context.cpp
        src/graphics/vulkan/command_queue.cpp
        src/graphics/vulkan/memory.cpp
        src/graphics/vulkan/guest_memory_mirror.cpp
        src/graphics/vulkan/resource_layout.cpp
        src/graphics/vulkan/pipeline.cpp
        src/graphics/vulkan/render_target.cpp
        src/graphics/vulkan/depth_target.cpp
        src/graphics/vulkan/texture.cpp
        src/graphics/vulkan/presentation.cpp
        src/graphics/vulkan/backend.cpp)
      target_include_directories(xenon_graphics_vulkan PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
      target_link_libraries(xenon_graphics_vulkan PUBLIC xenon_graphics ${XENON_VULKAN_TARGET})
      if(TARGET xenon_graphics_dxc)
        target_link_libraries(xenon_graphics_vulkan PRIVATE xenon_graphics_dxc)
        target_compile_definitions(xenon_graphics_vulkan PRIVATE XENON_HAS_DXC=1)
      endif()
      target_compile_features(xenon_graphics_vulkan PUBLIC cxx_std_20)
      xenon_enable_warnings(xenon_graphics_vulkan)
      add_library(Xenon::GraphicsVulkan ALIAS xenon_graphics_vulkan)
    else()
      message(STATUS "Vulkan runtime backend disabled: no compatible Vulkan loader/headers were resolved")
    endif()
  endif()

  if(WIN32 AND XENON_ENABLE_D3D12)
    add_library(xenon_graphics_d3d12 STATIC
      src/graphics/d3d12/context.cpp
      src/graphics/d3d12/command_queue.cpp
      src/graphics/d3d12/memory.cpp
      src/graphics/d3d12/guest_memory_mirror.cpp
      src/graphics/d3d12/resource_layout.cpp
      src/graphics/d3d12/pipeline.cpp
      src/graphics/d3d12/render_target.cpp
      src/graphics/d3d12/depth_target.cpp
      src/graphics/d3d12/texture.cpp
      src/graphics/d3d12/presentation.cpp
      src/graphics/d3d12/backend.cpp)
    target_include_directories(xenon_graphics_d3d12 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(xenon_graphics_d3d12 PUBLIC xenon_graphics d3d12 dxgi dxguid)
    if(TARGET xenon_graphics_dxc)
      target_link_libraries(xenon_graphics_d3d12 PRIVATE xenon_graphics_dxc)
      target_compile_definitions(xenon_graphics_d3d12 PRIVATE XENON_HAS_DXC=1)
    endif()
    target_compile_features(xenon_graphics_d3d12 PUBLIC cxx_std_20)
    xenon_enable_warnings(xenon_graphics_d3d12)
    add_library(Xenon::GraphicsD3D12 ALIAS xenon_graphics_d3d12)
  endif()
endif()

# -----------------------------------------------------------------------------
# xenon_core's real subsystem links. Every subsystem target xenon_core's own
# source (XenonSession, in particular) actually calls into now exists above
# this point, so these if(TARGET ...) checks are meaningful here (see the
# comment left where this used to live, right after add_library(xenon_core
# ...): placed there, every check saw the target as not-yet-defined and
# silently linked nothing.
if(TARGET xenon_memory)
  target_link_libraries(xenon_core PUBLIC xenon_memory)
endif()
if(TARGET xenon_cpu)
  target_link_libraries(xenon_core PUBLIC xenon_cpu)
endif()
if(TARGET xenon_filesystem)
  target_link_libraries(xenon_core PUBLIC xenon_filesystem)
endif()
if(TARGET xenon_kernel)
  target_link_libraries(xenon_core PUBLIC xenon_kernel)
endif()
if(TARGET xenon_input)
  target_link_libraries(xenon_core PUBLIC xenon_input)
endif()
# XenonSession::init_exports() registers input::xam::guest::GuestInputBridge's
# dispatch() into the unified ExportRegistry (see src/core/session.cpp), so it
# needs the guest ABI bridge linked in whenever it was built.
if(TARGET xenon_input_guest)
  target_link_libraries(xenon_core PUBLIC xenon_input_guest)
endif()
if(TARGET xenon_graphics)
  target_link_libraries(xenon_core PUBLIC xenon_graphics)
endif()
# XenonSession::init_gpu() selects and constructs the real native backend
# directly (see src/core/session.cpp) rather than going through a separate
# factory/plugin layer, so it needs these compiled in whenever they exist.
if(TARGET xenon_graphics_vulkan)
  target_link_libraries(xenon_core PUBLIC xenon_graphics_vulkan)
  target_compile_definitions(xenon_core PUBLIC XENON_HAS_VULKAN=1)
endif()
if(TARGET xenon_graphics_d3d12)
  target_link_libraries(xenon_core PUBLIC xenon_graphics_d3d12)
  target_compile_definitions(xenon_core PUBLIC XENON_HAS_D3D12=1)
endif()
if(TARGET xenon_audio)
  target_link_libraries(xenon_core PUBLIC xenon_audio)
endif()
# XenonSession loads XEX images and dynamically loads native module
# extensions directly (see include/xenon/core/session.hpp), so it needs the
# guest-facing XEX loader/import registry regardless of the host-side
# xboxkrnl I/O export wiring living in the same target.
if(TARGET xenon_xbox_kernel_io)
  target_link_libraries(xenon_core PUBLIC xenon_xbox_kernel_io)
endif()

# -----------------------------------------------------------------------------
# Tests
if(XENON_BUILD_TESTS)
  enable_testing()

  find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/deps/bootstrap_tests.py")
    add_test(NAME xenon_dependency_bootstrap_tests
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/deps/bootstrap_tests.py")
    set_tests_properties(xenon_dependency_bootstrap_tests PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
  if(Python3_Interpreter_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/release/verify_install_tests.py")
    add_test(NAME xenon_release_verify_install_tests
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/release/verify_install_tests.py")
    set_tests_properties(xenon_release_verify_install_tests PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()

  # Core session tests
  add_executable(xenon_session_tests tests/core/session_tests.cpp)
  target_link_libraries(xenon_session_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_session_tests)
  add_test(NAME xenon_session_tests COMMAND xenon_session_tests)

  # Phase 0 (AC6 Runtime Readiness pass): shared telemetry/diagnostics
  # foundation - CapabilityReportBuilder aggregation, the Logger's
  # cheap-when-disabled gate, and RunFingerprint equality/serialization.
  add_executable(xenon_capability_report_tests tests/core/capability_report_tests.cpp)
  target_link_libraries(xenon_capability_report_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_capability_report_tests)
  add_test(NAME xenon_capability_report_tests COMMAND xenon_capability_report_tests)

  add_executable(xenon_run_fingerprint_tests tests/core/run_fingerprint_tests.cpp)
  target_link_libraries(xenon_run_fingerprint_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_run_fingerprint_tests)
  add_test(NAME xenon_run_fingerprint_tests COMMAND xenon_run_fingerprint_tests)

  if(TARGET xenon_network)
    add_executable(xenon_network_tests tests/network/network_tests.cpp)
    target_link_libraries(xenon_network_tests PRIVATE Xenon::Network)
    target_compile_definitions(xenon_network_tests PRIVATE
      XENON_NETWORK_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/network/fixtures")
    xenon_enable_test_asserts(xenon_network_tests)
    add_test(NAME xenon_network_tests COMMAND xenon_network_tests)
  endif()

  add_executable(xenon_session_execution_tests
    tests/core/session_execution_tests.cpp)
  target_link_libraries(xenon_session_execution_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_session_execution_tests)
  add_test(NAME xenon_session_execution_tests COMMAND xenon_session_execution_tests)

  # Phase 1/2 (AC6 Runtime Readiness pass): ExCreateThread, the shared
  # dispatch_guest_thread()/run_created_guest_thread() primitive, and the
  # external_call() real-calling-thread-id regression.
  add_executable(xenon_thread_creation_tests tests/core/thread_creation_tests.cpp)
  target_link_libraries(xenon_thread_creation_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_thread_creation_tests)
  add_test(NAME xenon_thread_creation_tests COMMAND xenon_thread_creation_tests)

  # Phase 6 (AC6 Runtime Readiness pass): whole-XEX import capability audit
  # four-state classification (IMPLEMENTED/SAFE_STUB/PARTIAL/MISSING).
  add_executable(xenon_import_capability_report_tests tests/core/import_capability_report_tests.cpp)
  target_link_libraries(xenon_import_capability_report_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_import_capability_report_tests)
  add_test(NAME xenon_import_capability_report_tests COMMAND xenon_import_capability_report_tests)

  if(TARGET xenon_audio)
    add_executable(xenon_audio_mixer_tests tests/audio/mixer_tests.cpp)
    target_link_libraries(xenon_audio_mixer_tests PRIVATE xenon_audio)
    xenon_enable_test_asserts(xenon_audio_mixer_tests)
    xenon_stage_runtime_dependencies(xenon_audio_mixer_tests)
    add_test(NAME xenon_audio_mixer_tests COMMAND xenon_audio_mixer_tests)

    add_executable(xenon_audio_xma_tests tests/audio/xma_tests.cpp)
    target_link_libraries(xenon_audio_xma_tests PRIVATE xenon_audio)
    xenon_enable_test_asserts(xenon_audio_xma_tests)
    xenon_stage_runtime_dependencies(xenon_audio_xma_tests)
    add_test(NAME xenon_audio_xma_tests COMMAND xenon_audio_xma_tests)

    add_executable(xenon_audio_system_tests tests/audio/system_tests.cpp)
    target_link_libraries(xenon_audio_system_tests PRIVATE xenon_audio)
    xenon_enable_test_asserts(xenon_audio_system_tests)
    xenon_stage_runtime_dependencies(xenon_audio_system_tests)
    add_test(NAME xenon_audio_system_tests COMMAND xenon_audio_system_tests)

    # src/audio/exports.cpp is already compiled into xenon_core when Audio V1
    # is enabled; do not add it here again or static links gain duplicate
    # export-registration symbols.
    add_executable(xenon_audio_export_tests tests/audio/export_tests.cpp)
    target_link_libraries(xenon_audio_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_audio_export_tests)
    xenon_stage_runtime_dependencies(xenon_audio_export_tests)
    add_test(NAME xenon_audio_export_tests COMMAND xenon_audio_export_tests)
  endif()

  if(XENON_ENABLE_FILESYSTEM)
    add_executable(xenon_filesystem_tests tests/filesystem/filesystem_tests.cpp)
    target_link_libraries(xenon_filesystem_tests PRIVATE xenon_filesystem)
    xenon_enable_test_asserts(xenon_filesystem_tests)
    add_test(NAME xenon_filesystem_tests COMMAND xenon_filesystem_tests)
  endif()

  if(XENON_ENABLE_KERNEL)
    if(XENON_ENABLE_MEMORY)
      add_executable(xenon_guest_heap_tests tests/kernel/guest_heap_tests.cpp)
      target_link_libraries(xenon_guest_heap_tests PRIVATE xenon_kernel)
      xenon_enable_test_asserts(xenon_guest_heap_tests)
      add_test(NAME xenon_guest_heap_tests COMMAND xenon_guest_heap_tests)
    endif()

    add_executable(xenon_kernel_io_tests tests/kernel/kernel_io_tests.cpp)
    target_link_libraries(xenon_kernel_io_tests PRIVATE xenon_kernel)
    xenon_enable_test_asserts(xenon_kernel_io_tests)
    add_test(NAME xenon_kernel_io_tests COMMAND xenon_kernel_io_tests)

    # Phase 3 (AC6 Runtime Readiness pass): mutant owner-id-1 collision and
    # wait_on_object() mutant thread-identity regressions.
    add_executable(xenon_kernel_synchronization_tests tests/kernel/synchronization_tests.cpp)
    target_link_libraries(xenon_kernel_synchronization_tests PRIVATE xenon_kernel)
    xenon_enable_test_asserts(xenon_kernel_synchronization_tests)
    add_test(NAME xenon_kernel_synchronization_tests COMMAND xenon_kernel_synchronization_tests)

    # Phase 3 (AC6 Runtime Readiness pass): real timer-dispatch thread -
    # a timer with due_time > 0 now actually fires, periodic re-arm works,
    # cancel prevents firing, and teardown does not hang.
    add_executable(xenon_kernel_timer_dispatch_tests tests/kernel/timer_dispatch_tests.cpp)
    target_link_libraries(xenon_kernel_timer_dispatch_tests PRIVATE xenon_kernel)
    xenon_enable_test_asserts(xenon_kernel_timer_dispatch_tests)
    add_test(NAME xenon_kernel_timer_dispatch_tests COMMAND xenon_kernel_timer_dispatch_tests)

    # Phase 5 (AC6 Runtime Readiness pass): ExceptionDispatcher::fault_to_exception()
    # Execute-fault/ExceptionInformation[0] regression.
    add_executable(xenon_kernel_exception_tests tests/kernel/exception_tests.cpp)
    target_link_libraries(xenon_kernel_exception_tests PRIVATE xenon_kernel)
    xenon_enable_test_asserts(xenon_kernel_exception_tests)
    add_test(NAME xenon_kernel_exception_tests COMMAND xenon_kernel_exception_tests)

    # Phase 2 (AC6 Runtime Readiness pass): join(timeout) real-timeout,
    # terminate()-vs-natural-return exit-code race, and CREATE_SUSPENDED
    # regressions.
    add_executable(xenon_kernel_thread_lifecycle_tests tests/kernel/thread_lifecycle_tests.cpp)
    target_link_libraries(xenon_kernel_thread_lifecycle_tests PRIVATE xenon_kernel)
    xenon_enable_test_asserts(xenon_kernel_thread_lifecycle_tests)
    add_test(NAME xenon_kernel_thread_lifecycle_tests COMMAND xenon_kernel_thread_lifecycle_tests)
  endif()

  if(TARGET xenon_xbox_kernel_io)
    add_executable(xenon_xbox_guest_io_tests tests/kernel/xbox_guest_io_tests.cpp)
    target_link_libraries(xenon_xbox_guest_io_tests PRIVATE xenon_xbox_kernel_io)
    xenon_enable_test_asserts(xenon_xbox_guest_io_tests)
    add_test(NAME xenon_xbox_guest_io_tests COMMAND xenon_xbox_guest_io_tests)
  endif()

  if(TARGET xenon_xbox_kernel_io)
    add_executable(xenon_xbox_import_tests tests/xbox/xbox_import_tests.cpp)
    target_link_libraries(xenon_xbox_import_tests PRIVATE xenon_xbox_kernel_io)
    xenon_enable_test_asserts(xenon_xbox_import_tests)
    add_test(NAME xenon_xbox_import_tests COMMAND xenon_xbox_import_tests)

    add_executable(xenon_xex_loader_tests tests/xbox/xex_loader_tests.cpp)
    target_link_libraries(xenon_xex_loader_tests PRIVATE xenon_xbox_kernel_io)
    xenon_enable_test_asserts(xenon_xex_loader_tests)
    add_test(NAME xenon_xex_loader_tests COMMAND xenon_xex_loader_tests)

    add_executable(xenon_xex_crypto_tests tests/xbox/xex_crypto_tests.cpp)
    target_link_libraries(xenon_xex_crypto_tests PRIVATE xenon_xbox_kernel_io)
    xenon_enable_test_asserts(xenon_xex_crypto_tests)
    add_test(NAME xenon_xex_crypto_tests COMMAND xenon_xex_crypto_tests)

    add_executable(xenon_xex_lzx_tests tests/xbox/xex_lzx_tests.cpp)
    target_link_libraries(xenon_xex_lzx_tests PRIVATE xenon_xbox_kernel_io)
    xenon_enable_test_asserts(xenon_xex_lzx_tests)
    add_test(NAME xenon_xex_lzx_tests COMMAND xenon_xex_lzx_tests)

    add_executable(xenon_rtl_tests tests/xbox/rtl_tests.cpp)
    target_link_libraries(xenon_rtl_tests PRIVATE xenon_xbox_kernel_io)
    xenon_enable_test_asserts(xenon_rtl_tests)
    add_test(NAME xenon_rtl_tests COMMAND xenon_rtl_tests)
  endif()

  # Phase 1/4 (AC6 Runtime Readiness pass): xboxkrnl time exports, driven
  # through the real core::ExportRegistry - link xenon_core (which links
  # xenon_xbox_kernel_io PUBLIC) rather than xenon_xbox_kernel_io alone, since
  # ExportRegistry's out-of-line methods live in xenon_core's own sources
  # (see xenon_audio_export_tests above for the same pattern).
  if(TARGET xenon_core)
    add_executable(xenon_xboxkrnl_time_export_tests tests/xbox/time_export_tests.cpp)
    target_link_libraries(xenon_xboxkrnl_time_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_xboxkrnl_time_export_tests)
    add_test(NAME xenon_xboxkrnl_time_export_tests COMMAND xenon_xboxkrnl_time_export_tests)
  endif()

  # Phase 1/3 (AC6 Runtime Readiness pass): xboxkrnl handle-based (Nt*) sync
  # exports - NtCreateEvent/Semaphore/Mutant, release, and the Ex wait
  # variants - driven through the real core::ExportRegistry against a real
  # kernel::KernelProcess. Links xenon_core (which links xenon_xbox_kernel_io
  # PUBLIC) rather than xenon_xbox_kernel_io alone: ExportRegistry's
  # out-of-line methods live in xenon_core's own sources, and a STATIC
  # library like xenon_xbox_kernel_io does not itself resolve the symbols its
  # translation units reference - see xenon_xboxkrnl_time_export_tests above
  # for the identical reasoning.
  if(TARGET xenon_core)
    add_executable(xenon_xboxkrnl_sync_export_tests tests/xbox/sync_export_tests.cpp)
    target_link_libraries(xenon_xboxkrnl_sync_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_xboxkrnl_sync_export_tests)
    add_test(NAME xenon_xboxkrnl_sync_export_tests COMMAND xenon_xboxkrnl_sync_export_tests)
  endif()

  add_executable(xenon_recomp_driver_tests tests/recomp/recomp_driver_tests.cpp)
  target_link_libraries(xenon_recomp_driver_tests PRIVATE xenon_recomp)
  target_compile_definitions(xenon_recomp_driver_tests PRIVATE
    XENON_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
  xenon_enable_test_asserts(xenon_recomp_driver_tests)
  add_test(NAME xenon_recomp_driver_tests COMMAND xenon_recomp_driver_tests)

  # Generic worker pool (Part 3 of the Recomp Analysis V2 pass): correctness
  # of parallel_for() itself (every index run exactly once, result ordering
  # independent of completion order, exception propagation, no races under
  # repeated stress runs), independent of the Recomp Driver that consumes it.
  add_executable(xenon_worker_pool_tests tests/recomp/worker_pool_tests.cpp)
  target_link_libraries(xenon_worker_pool_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_worker_pool_tests)
  add_test(NAME xenon_worker_pool_tests COMMAND xenon_worker_pool_tests)

  # Parallel analysis determinism (Part 4/21): jobs=1 vs jobs=N produce
  # byte-identical deterministic AnalysisReport content on a synthetic
  # multi-function fixture, and discovery waves converge correctly.
  add_executable(xenon_recomp_parallel_analysis_tests tests/recomp/parallel_analysis_tests.cpp)
  target_link_libraries(xenon_recomp_parallel_analysis_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_recomp_parallel_analysis_tests)
  add_test(NAME xenon_recomp_parallel_analysis_tests COMMAND xenon_recomp_parallel_analysis_tests)

  # Recomp Analysis V3 (discovery-quality pass): provenance/evidence
  # combination, confidence, generic TLS-callback seeding, bounded CTR
  # dataflow resolution, invalid-vs-unsupported PPC classification, the
  # conservative prologue heuristic, validated tail calls, and
  # switch/jump-table bounds-check gating.
  add_executable(xenon_discovery_quality_tests tests/recomp/discovery_quality_tests.cpp)
  target_link_libraries(xenon_discovery_quality_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_discovery_quality_tests)
  add_test(NAME xenon_discovery_quality_tests COMMAND xenon_discovery_quality_tests)

  # Gen 6 PPC value/indirect-flow analysis: bounded abstract-value lattice,
  # cheap block-local propagation, read-only pointer/vtable folding, and the
  # selective bounded backward-slice resolver.
  add_executable(xenon_ppc_value_analysis_tests tests/recomp/ppc_value_analysis_tests.cpp)
  target_link_libraries(xenon_ppc_value_analysis_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_ppc_value_analysis_tests)
  add_test(NAME xenon_ppc_value_analysis_tests COMMAND xenon_ppc_value_analysis_tests)

  # Gen 9 universal knowledge base: versioned normalized fingerprints,
  # cross-revision matching, confidence gates, JSONL persistence and replay.
  find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
  add_test(NAME xenon_native_cache_tests COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/recomp/native_cache_tests.py")
  set_tests_properties(xenon_native_cache_tests PROPERTIES ENVIRONMENT "XENON_TEST_CXX=${CMAKE_CXX_COMPILER}")
  add_executable(xenon_compilation_graph_tests tests/recomp/compilation_graph_tests.cpp)
  target_link_libraries(xenon_compilation_graph_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_compilation_graph_tests)
  add_test(NAME xenon_compilation_graph_tests COMMAND xenon_compilation_graph_tests)

  # Gen 11 autonomous game intake: multi-executable discovery over a
  # directory, a loose .xex file and a synthetic disc image, plus the Game
  # Compilation Graph's JSON shape.
  add_executable(xenon_game_intake_tests tests/recomp/game_intake_tests.cpp)
  target_link_libraries(xenon_game_intake_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_game_intake_tests)
  add_test(NAME xenon_game_intake_tests COMMAND xenon_game_intake_tests)

  add_executable(xenon_knowledge_base_tests tests/recomp/knowledge_base_tests.cpp)
  target_link_libraries(xenon_knowledge_base_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_knowledge_base_tests)
  add_test(NAME xenon_knowledge_base_tests COMMAND xenon_knowledge_base_tests)

  # Prepared native-module artifact cache (automatic game preparation
  # pipeline): digest/invalidation semantics and real stage->validate->
  # atomic-promote/rollback behaviour, including a real compiled-and-loaded
  # shared library check (not a stubbed file-existence check).
  add_executable(xenon_artifact_cache_tests tests/recomp/artifact_cache_tests.cpp)
  target_link_libraries(xenon_artifact_cache_tests PRIVATE xenon_recomp)
  if(NOT WIN32)
    target_link_libraries(xenon_artifact_cache_tests PRIVATE ${CMAKE_DL_LIBS})
  endif()
  xenon_enable_test_asserts(xenon_artifact_cache_tests)
  add_test(NAME xenon_artifact_cache_tests COMMAND xenon_artifact_cache_tests)

  # End-to-end xenon-prepare worker tests: drives the real xenon-prepare
  # binary as a subprocess (first build, no-rebuild, hint-change rebuild,
  # deterministic cancellation) - see tools/xenon_prepare.cpp and
  # docs/development/GAME_PREPARATION.md.
  add_executable(xenon_prepare_worker_tests tests/recomp/xenon_prepare_worker_tests.cpp)
  target_link_libraries(xenon_prepare_worker_tests PRIVATE xenon_recomp)
  target_compile_definitions(xenon_prepare_worker_tests PRIVATE
    XENON_PREPARE_EXECUTABLE="$<TARGET_FILE:xenon-prepare>"
    XENON_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
    XENON_NATIVE_COMPILER="${CMAKE_CXX_COMPILER}"
    XENON_CMAKE_COMMAND="${CMAKE_COMMAND}")
  add_dependencies(xenon_prepare_worker_tests xenon-prepare)
  xenon_enable_test_asserts(xenon_prepare_worker_tests)
  add_test(NAME xenon_prepare_worker_tests COMMAND xenon_prepare_worker_tests)

  # Analysis Hint Schema V2 (Part 1/7 of the Gracemeria readiness pass):
  # validate_hint_set()/hint_set_matches_identity()/JSON round trip, fully
  # independent of any XEX/driver pipeline.
  add_executable(xenon_analysis_schema_tests tests/recomp/analysis_schema_tests.cpp)
  target_link_libraries(xenon_analysis_schema_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_analysis_schema_tests)
  add_test(NAME xenon_analysis_schema_tests COMMAND xenon_analysis_schema_tests)

  # Analysis Hint Schema V2 consumption: proves the Recomp Driver actually
  # uses chunks/switch tables/known indirect calls/native replacements/
  # regions during real analysis, not merely parses/stores them (Part 1.11).
  add_executable(xenon_heap_native_replacement_tests
    tests/recomp/heap_native_replacement_tests.cpp)
  target_link_libraries(xenon_heap_native_replacement_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_heap_native_replacement_tests)
  add_test(NAME xenon_heap_native_replacement_tests
    COMMAND xenon_heap_native_replacement_tests)

  add_executable(xenon_analysis_v2_consumption_tests tests/recomp/analysis_v2_consumption_tests.cpp)
  target_link_libraries(xenon_analysis_v2_consumption_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_analysis_v2_consumption_tests)
  add_test(NAME xenon_analysis_v2_consumption_tests COMMAND xenon_analysis_v2_consumption_tests)

  # Generated-code deduplication / shard ownership fix: proves the actual
  # production bug (byte-identical functions colliding in the codegen
  # content-addressed cache, producing MSVC C2084 duplicate-body errors on a
  # real title) stays fixed, plus the FunctionChunk/independent-FunctionHint
  # mutual-exclusion fix, parallel codegen determinism, and the
  # fail-fast symbol/address uniqueness validation added alongside it.
  add_executable(xenon_codegen_ownership_tests tests/recomp/codegen_ownership_tests.cpp)
  target_link_libraries(xenon_codegen_ownership_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_codegen_ownership_tests)
  add_test(NAME xenon_codegen_ownership_tests COMMAND xenon_codegen_ownership_tests)

  # Generated registry.cpp numeric-format state leak fix: proves
  # generate_project()'s trailing kCompiledFunctionCount declaration is always
  # emitted in decimal - never inheriting a std::hex formatting flag left
  # active by the guest-address table emitted immediately before it (the
  # real Ace Combat 6 build failure: kCompiledFunctionCount = 4b21; for a
  # 19233-function module) - including the CRITICAL case where a leaked hex
  # formatter would silently compile to a WRONG-but-valid decimal literal
  # (16 == 0x10) instead of failing loudly.
  add_executable(xenon_registry_numeric_format_tests tests/recomp/registry_numeric_format_tests.cpp)
  target_link_libraries(xenon_registry_numeric_format_tests PRIVATE xenon_recomp)
  target_compile_definitions(xenon_registry_numeric_format_tests PRIVATE
    XENON_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
  xenon_enable_test_asserts(xenon_registry_numeric_format_tests)
  add_test(NAME xenon_registry_numeric_format_tests COMMAND xenon_registry_numeric_format_tests)

  # ModuleHintProvider production integration (Part 2/7): a synthetic
  # two-revision module package, correct/incorrect revision selection, and
  # proof the provider is actually consumed by load_and_analyze().
  add_executable(xenon_module_hint_provider_tests tests/recomp/module_hint_provider_tests.cpp)
  target_link_libraries(xenon_module_hint_provider_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_module_hint_provider_tests)
  add_test(NAME xenon_module_hint_provider_tests COMMAND xenon_module_hint_provider_tests)

  # Proves the real "recompiled PPC -> import call -> CPU V2 -> external-call
  # bridge -> core::ExportRegistry -> active subsystem" path through the
  # actual production pipeline (XEX Loader V2, Recomp Driver, a compiled and
  # loaded native module, KernelProcess/KernelThread) - not a direct C++
  # shortcut. See tests/core/guest_export_abi_tests.cpp's header comment.
  add_executable(xenon_guest_export_abi_tests tests/core/guest_export_abi_tests.cpp)
  target_link_libraries(xenon_guest_export_abi_tests PRIVATE xenon_core xenon_recomp)
  target_compile_definitions(xenon_guest_export_abi_tests PRIVATE
    XENON_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
  xenon_enable_test_asserts(xenon_guest_export_abi_tests)
  add_test(NAME xenon_guest_export_abi_tests COMMAND xenon_guest_export_abi_tests)

  # Part 4 (Gracemeria readiness pass): proves the audio render-driver guest
  # callback runs with a real KernelProcess/KernelThread/KPCR/TLS identity
  # (XenonSession::start_audio_guest_thread()/invoke_audio_callback()) rather
  # than an isolated bare CpuState. See the file's header comment.
  add_executable(xenon_audio_guest_callback_tests tests/core/audio_guest_callback_tests.cpp)
  target_link_libraries(xenon_audio_guest_callback_tests PRIVATE xenon_core xenon_recomp)
  target_compile_definitions(xenon_audio_guest_callback_tests PRIVATE
    XENON_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
  xenon_enable_test_asserts(xenon_audio_guest_callback_tests)
  add_test(NAME xenon_audio_guest_callback_tests COMMAND xenon_audio_guest_callback_tests)

  # Negative-path coverage for the same import/export dispatch boundary:
  # unknown library, known library/missing ordinal, a malformed native
  # import table entry, a call target that is neither compiled code nor a
  # known import, a missing/incompatible native module, an import-identity
  # (ordinal-collision) regression, and a KernelProcess/KernelMemory/guest-
  # stack rollback proof. See the file's header comment for why these use
  # the production import-resolution boundary directly rather than a second
  # full compiled-module build pipeline.
  add_executable(xenon_guest_import_negative_tests tests/core/guest_import_negative_tests.cpp)
  target_link_libraries(xenon_guest_import_negative_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_guest_import_negative_tests)
  add_test(NAME xenon_guest_import_negative_tests COMMAND xenon_guest_import_negative_tests)

  # Minimal native "game module" used only by
  # tests/core/title_update_integration_tests.cpp to prove XenonSession's
  # effective-executable-revision compatibility gating through the real
  # LoadLibrary/dlopen native-extension path - see that fixture's own header
  # comment. It exports no real compiled code (never bound to a running
  # guest thread); it is not a game module and ships no game logic.
  add_library(xenon_title_update_test_module SHARED tests/core/fixtures/title_update_test_module.cpp)
  target_link_libraries(xenon_title_update_test_module PRIVATE xenon_cpu)

  # Title Update Runtime Integration: proves a selected title update reaches
  # XenonSession::load_game()'s real xbox::apply_title_update() call (not
  # XEX Loader V2 called directly), that the effective (patched) image - not
  # the base image - is what gets mapped/run, that malformed/incompatible
  # updates fail explicitly with no partial guest-process state, and that a
  # native extension's declared effective-executable-revision compatibility
  # is actually enforced. See the file's header comment.
  add_executable(xenon_title_update_integration_tests tests/core/title_update_integration_tests.cpp)
  target_link_libraries(xenon_title_update_integration_tests PRIVATE xenon_core)
  add_dependencies(xenon_title_update_integration_tests xenon_title_update_test_module)
  target_compile_definitions(xenon_title_update_integration_tests PRIVATE
    XENON_TITLE_UPDATE_TEST_MODULE_PATH="$<TARGET_FILE:xenon_title_update_test_module>")
  xenon_enable_test_asserts(xenon_title_update_integration_tests)
  add_test(NAME xenon_title_update_integration_tests COMMAND xenon_title_update_integration_tests)

  add_executable(xenon_cpu_tests tests/cpu/ppc/cpu_tests.cpp)
  target_link_libraries(xenon_cpu_tests PRIVATE xenon_cpu)
  target_include_directories(xenon_cpu_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/ppc")
  xenon_enable_test_asserts(xenon_cpu_tests)
  add_test(NAME xenon_cpu_tests COMMAND xenon_cpu_tests)

  add_executable(xenon_cpu_v2_cfg tests/cpu/ir/cpu_v2_cfg.cpp)
  target_link_libraries(xenon_cpu_v2_cfg PRIVATE xenon_cpu)
  xenon_enable_test_asserts(xenon_cpu_v2_cfg)
  add_test(NAME xenon_cpu_v2_cfg COMMAND xenon_cpu_v2_cfg)

  add_executable(xenon_cpu_v2_ir tests/cpu/ir/cpu_v2_ir.cpp)
  target_link_libraries(xenon_cpu_v2_ir PRIVATE xenon_cpu)
  xenon_enable_test_asserts(xenon_cpu_v2_ir)
  add_test(NAME xenon_cpu_v2_ir COMMAND xenon_cpu_v2_ir)

  add_executable(xenon_cpu_v2_state_promotion tests/cpu/optimizer/cpu_v2_state_promotion.cpp)
  target_link_libraries(xenon_cpu_v2_state_promotion PRIVATE xenon_cpu)
  xenon_enable_test_asserts(xenon_cpu_v2_state_promotion)
  add_test(NAME xenon_cpu_v2_state_promotion COMMAND xenon_cpu_v2_state_promotion)

  # Generate and compile one function for every canonical instruction.
  add_executable(xenon_cpu_codegen_all tests/cpu/codegen/codegen_all.cpp)
  target_link_libraries(xenon_cpu_codegen_all PRIVATE xenon_cpu)
  set(XENON_GENERATED_ALL "${CMAKE_CURRENT_BINARY_DIR}/generated_all_opcodes.cpp")
  add_custom_command(
    OUTPUT "${XENON_GENERATED_ALL}"
    COMMAND xenon_cpu_codegen_all "${XENON_GENERATED_ALL}"
    DEPENDS xenon_cpu_codegen_all
    VERBATIM)
  add_library(xenon_cpu_generated_compile OBJECT "${XENON_GENERATED_ALL}")
  target_include_directories(xenon_cpu_generated_compile PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_compile_features(xenon_cpu_generated_compile PRIVATE cxx_std_20)
  xenon_enable_guest_fp_semantics(xenon_cpu_generated_compile PRIVATE)
  add_dependencies(xenon_cpu_generated_compile xenon_cpu_codegen_all)

  add_executable(xenon_cpu_codegen_smoke tests/cpu/codegen/codegen_smoke.cpp)
  target_link_libraries(xenon_cpu_codegen_smoke PRIVATE xenon_cpu)
  set(XENON_GENERATED_SMOKE "${CMAKE_CURRENT_BINARY_DIR}/generated_native_smoke.cpp")
  add_custom_command(
    OUTPUT "${XENON_GENERATED_SMOKE}"
    COMMAND xenon_cpu_codegen_smoke "${XENON_GENERATED_SMOKE}"
    DEPENDS xenon_cpu_codegen_smoke
    VERBATIM)
  add_executable(xenon_cpu_native_smoke
    tests/cpu/x86_64/native_smoke.cpp
    "${XENON_GENERATED_SMOKE}")
  target_link_libraries(xenon_cpu_native_smoke PRIVATE xenon_cpu)
  xenon_enable_test_asserts(xenon_cpu_native_smoke)
  add_test(NAME xenon_cpu_native_smoke COMMAND xenon_cpu_native_smoke)

  add_executable(xenon_cpu_exhaustive_native
    tests/cpu/x86_64/exhaustive_native.cpp
    $<TARGET_OBJECTS:xenon_cpu_generated_compile>)
  target_link_libraries(xenon_cpu_exhaustive_native PRIVATE xenon_cpu)
  add_dependencies(xenon_cpu_exhaustive_native xenon_cpu_generated_compile)
  xenon_enable_test_asserts(xenon_cpu_exhaustive_native)
  add_test(NAME xenon_cpu_exhaustive_native COMMAND xenon_cpu_exhaustive_native)

  function(xenon_add_generated_cpu_test name generator source)
    add_executable(xenon_cpu_codegen_${name} "tests/cpu/codegen/${generator}")
    target_link_libraries(xenon_cpu_codegen_${name} PRIVATE xenon_cpu)
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated_${name}.cpp")
    add_custom_command(
      OUTPUT "${generated}"
      COMMAND xenon_cpu_codegen_${name} "${generated}"
      DEPENDS xenon_cpu_codegen_${name}
      VERBATIM)
    add_executable(xenon_cpu_${name} "tests/cpu/x86_64/${source}" "${generated}")
    target_link_libraries(xenon_cpu_${name} PRIVATE xenon_cpu)
    xenon_enable_test_asserts(xenon_cpu_${name})
    add_test(NAME xenon_cpu_${name} COMMAND xenon_cpu_${name})
  endfunction()

  add_executable(xenon_cpu_codegen_setjmp_longjmp
    tests/cpu/codegen/codegen_setjmp_longjmp.cpp)
  target_link_libraries(xenon_cpu_codegen_setjmp_longjmp PRIVATE xenon_cpu)
  set(XENON_GENERATED_SETJMP_LONGJMP
    "${CMAKE_CURRENT_BINARY_DIR}/generated_setjmp_longjmp.cpp")
  add_custom_command(
    OUTPUT "${XENON_GENERATED_SETJMP_LONGJMP}"
    COMMAND xenon_cpu_codegen_setjmp_longjmp "${XENON_GENERATED_SETJMP_LONGJMP}"
    DEPENDS xenon_cpu_codegen_setjmp_longjmp
    VERBATIM)
  add_executable(xenon_cpu_setjmp_longjmp
    tests/cpu/x86_64/setjmp_longjmp.cpp
    "${XENON_GENERATED_SETJMP_LONGJMP}")
  target_link_libraries(xenon_cpu_setjmp_longjmp PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_cpu_setjmp_longjmp)
  add_test(NAME xenon_cpu_setjmp_longjmp COMMAND xenon_cpu_setjmp_longjmp)

  add_executable(xenon_cpu_runtime_helpers_register_range
    tests/cpu/x86_64/runtime_helpers_register_range.cpp)
  target_link_libraries(xenon_cpu_runtime_helpers_register_range PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_cpu_runtime_helpers_register_range)
  add_test(NAME xenon_cpu_runtime_helpers_register_range COMMAND xenon_cpu_runtime_helpers_register_range)

  # Gen 8 semantic verification: an independent PowerPC reference model is
  # differentially checked against real generated AOT code using deterministic
  # randomized CPU/memory states.  The framework test also proves exact-seed
  # replay, bounded failure minimization, and repro-artifact emission.
  add_executable(xenon_cpu_semantic_verifier_tests
    tests/cpu/verification/semantic_verifier_tests.cpp)
  target_link_libraries(xenon_cpu_semantic_verifier_tests PRIVATE xenon_cpu)
  xenon_enable_test_asserts(xenon_cpu_semantic_verifier_tests)
  add_test(NAME xenon_cpu_semantic_verifier_tests COMMAND xenon_cpu_semantic_verifier_tests)

  add_executable(xenon_cpu_codegen_semantic_verification
    tests/cpu/codegen/codegen_semantic_verification.cpp)
  target_include_directories(xenon_cpu_codegen_semantic_verification PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/cpu/verification")
  target_link_libraries(xenon_cpu_codegen_semantic_verification PRIVATE xenon_cpu)
  set(XENON_GENERATED_SEMANTIC_VERIFICATION
    "${CMAKE_CURRENT_BINARY_DIR}/generated_semantic_verification.cpp")
  add_custom_command(
    OUTPUT "${XENON_GENERATED_SEMANTIC_VERIFICATION}"
    COMMAND xenon_cpu_codegen_semantic_verification "${XENON_GENERATED_SEMANTIC_VERIFICATION}"
    DEPENDS xenon_cpu_codegen_semantic_verification
    VERBATIM)
  add_executable(xenon_cpu_semantic_verification
    tests/cpu/x86_64/semantic_verification.cpp
    "${XENON_GENERATED_SEMANTIC_VERIFICATION}")
  target_include_directories(xenon_cpu_semantic_verification PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/cpu/verification")
  target_link_libraries(xenon_cpu_semantic_verification PRIVATE xenon_cpu)
  xenon_enable_test_asserts(xenon_cpu_semantic_verification)
  add_test(NAME xenon_cpu_semantic_verification COMMAND xenon_cpu_semantic_verification)

  xenon_add_generated_cpu_test(function_flow codegen_function_flow.cpp function_flow.cpp)
  xenon_add_generated_cpu_test(branch_matrix codegen_branch_matrix.cpp branch_matrix.cpp)
  xenon_add_generated_cpu_test(control_boundaries codegen_control_boundaries.cpp control_boundaries.cpp)
  xenon_add_generated_cpu_test(vector_memory codegen_vector_memory.cpp vector_memory.cpp)
  xenon_add_generated_cpu_test(indirect_branch codegen_indirect_branch.cpp indirect_branch.cpp)
  xenon_add_generated_cpu_test(scalar_memory codegen_scalar_memory.cpp scalar_memory.cpp)
  xenon_add_generated_cpu_test(integer_edges codegen_integer_edges.cpp integer_edges.cpp)

  # XAM tests
  add_executable(xenon_xam_user_manager_tests tests/xam/user_manager_tests.cpp)
  target_link_libraries(xenon_xam_user_manager_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_xam_user_manager_tests)
  add_test(NAME xenon_xam_user_manager_tests COMMAND xenon_xam_user_manager_tests)

  add_executable(xenon_xam_export_ordinal_tests tests/xam/xam_export_ordinal_tests.cpp)
  target_link_libraries(xenon_xam_export_ordinal_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_xam_export_ordinal_tests)
  add_test(NAME xenon_xam_export_ordinal_tests COMMAND xenon_xam_export_ordinal_tests)

  if(XENON_ENABLE_INPUT)
    add_executable(xenon_input_tests tests/input/input_tests.cpp)
    target_link_libraries(xenon_input_tests PRIVATE xenon_input)
    xenon_enable_test_asserts(xenon_input_tests)
    add_test(NAME xenon_input_tests COMMAND xenon_input_tests)

    add_executable(xenon_frontend_router_tests tests/input/frontend_router_tests.cpp)
    target_link_libraries(xenon_frontend_router_tests PRIVATE xenon_input)
    xenon_enable_test_asserts(xenon_frontend_router_tests)
    add_test(NAME xenon_frontend_router_tests COMMAND xenon_frontend_router_tests)

    foreach(test_name IN ITEMS sdl_driver keyboard_mouse xam_facade profile advanced module_api)
      set(target "xenon_input_${test_name}_tests")
      add_executable(${target} "tests/input/${test_name}_tests.cpp")
      target_link_libraries(${target} PRIVATE xenon_input)
      xenon_enable_test_asserts(${target})
      add_test(NAME ${target} COMMAND ${target})
    endforeach()
    if(TARGET xenon_input_guest)
      add_executable(xenon_input_xam_guest_tests tests/input/xam_guest_tests.cpp)
      target_link_libraries(xenon_input_xam_guest_tests PRIVATE xenon_input_guest)
      xenon_enable_test_asserts(xenon_input_xam_guest_tests)
      add_test(NAME xenon_input_xam_guest_tests COMMAND xenon_input_xam_guest_tests)
    endif()
  endif()

  if(XENON_ENABLE_MEMORY)
    add_executable(xenon_memory_tests tests/memory/memory_tests.cpp)
    target_link_libraries(xenon_memory_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_memory_tests)
    add_test(NAME xenon_memory_tests COMMAND xenon_memory_tests)

    add_executable(xenon_memory_hardening_tests
      tests/memory/memory_hardening_tests.cpp)
    target_link_libraries(xenon_memory_hardening_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_memory_hardening_tests)
    add_test(NAME xenon_memory_hardening_tests
      COMMAND xenon_memory_hardening_tests)

    add_executable(xenon_memory_ordering_tests
      tests/memory/memory_ordering_tests.cpp)
    target_link_libraries(xenon_memory_ordering_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_memory_ordering_tests)
    add_test(NAME xenon_memory_ordering_tests COMMAND xenon_memory_ordering_tests)

    add_executable(xenon_memory_type_tests
      tests/memory/memory_type_tests.cpp)
    target_link_libraries(xenon_memory_type_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_memory_type_tests)
    add_test(NAME xenon_memory_type_tests COMMAND xenon_memory_type_tests)

    add_executable(xenon_mmio_tests
      tests/memory/mmio_tests.cpp)
    target_link_libraries(xenon_mmio_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_mmio_tests)
    add_test(NAME xenon_mmio_tests COMMAND xenon_mmio_tests)

    add_executable(xenon_memory_fault_tests
      tests/memory/fault_tests.cpp)
    target_link_libraries(xenon_memory_fault_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_memory_fault_tests)
    add_test(NAME xenon_memory_fault_tests COMMAND xenon_memory_fault_tests)

    add_executable(xenon_dynamic_fallback_tests
      tests/cpu/dynamic_fallback_tests.cpp)
    target_link_libraries(xenon_dynamic_fallback_tests PRIVATE xenon_cpu xenon_memory)
    xenon_enable_test_asserts(xenon_dynamic_fallback_tests)
    add_test(NAME xenon_dynamic_fallback_tests COMMAND xenon_dynamic_fallback_tests)

    add_executable(xenon_dynamic_fallback_semantic_tests
      tests/cpu/verification/dynamic_fallback_semantic_tests.cpp)
    target_include_directories(xenon_dynamic_fallback_semantic_tests PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/cpu/verification")
    target_link_libraries(xenon_dynamic_fallback_semantic_tests PRIVATE xenon_cpu xenon_memory)
    xenon_enable_test_asserts(xenon_dynamic_fallback_semantic_tests)
    add_test(NAME xenon_dynamic_fallback_semantic_tests COMMAND xenon_dynamic_fallback_semantic_tests)

    add_executable(xenon_executable_code_cache_tests
      tests/memory/executable_code_cache_tests.cpp)
    target_link_libraries(xenon_executable_code_cache_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_executable_code_cache_tests)
    add_test(NAME xenon_executable_code_cache_tests
      COMMAND xenon_executable_code_cache_tests)

    add_executable(xenon_host_vm_tests tests/memory/host_vm_tests.cpp)
    target_link_libraries(xenon_host_vm_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_host_vm_tests)
    add_test(NAME xenon_host_vm_tests COMMAND xenon_host_vm_tests)

    add_executable(xenon_gpu_coherency_tests
      tests/memory/gpu_coherency_tests.cpp)
    target_link_libraries(xenon_gpu_coherency_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_gpu_coherency_tests)
    add_test(NAME xenon_gpu_coherency_tests COMMAND xenon_gpu_coherency_tests)

    add_executable(xenon_memory_codegen_integration tests/memory/codegen_memory_integration.cpp)
    target_link_libraries(xenon_memory_codegen_integration PRIVATE xenon_cpu)
    set(XENON_GENERATED_MEMORY_INTEGRATION
      "${CMAKE_CURRENT_BINARY_DIR}/generated_memory_integration.cpp")
    add_custom_command(
      OUTPUT "${XENON_GENERATED_MEMORY_INTEGRATION}"
      COMMAND xenon_memory_codegen_integration "${XENON_GENERATED_MEMORY_INTEGRATION}"
      DEPENDS xenon_memory_codegen_integration
      VERBATIM)
    add_executable(xenon_memory_cpu_integration
      tests/memory/memory_cpu_integration.cpp
      "${XENON_GENERATED_MEMORY_INTEGRATION}")
    target_link_libraries(xenon_memory_cpu_integration PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_memory_cpu_integration)
    add_test(NAME xenon_memory_cpu_integration COMMAND xenon_memory_cpu_integration)
  endif()

  if(XENON_ENABLE_GRAPHICS)
    add_executable(xenon_backend_capability_tests
      tests/graphics/backends/backend_capabilities.cpp)
    set_target_properties(xenon_backend_capability_tests PROPERTIES
      OUTPUT_NAME xenon_native_backend_gpu10_tests)
    target_link_libraries(xenon_backend_capability_tests PRIVATE xenon_graphics)
    if(WIN32 AND TARGET xenon_graphics_d3d12)
      target_link_libraries(xenon_backend_capability_tests PRIVATE xenon_graphics_d3d12)
      target_compile_definitions(xenon_backend_capability_tests PRIVATE XENON_TEST_D3D12=1)
    endif()
    if(TARGET xenon_graphics_vulkan)
      target_link_libraries(xenon_backend_capability_tests PRIVATE xenon_graphics_vulkan)
      target_compile_definitions(xenon_backend_capability_tests PRIVATE XENON_TEST_VULKAN=1)
    endif()
    if(TARGET xenon_graphics_dxc)
      target_link_libraries(xenon_backend_capability_tests PRIVATE xenon_graphics_dxc)
      target_compile_definitions(xenon_backend_capability_tests PRIVATE XENON_TEST_DXC=1)
      xenon_stage_runtime_dependencies(xenon_backend_capability_tests)
    endif()
    xenon_enable_test_asserts(xenon_backend_capability_tests)
    add_test(NAME xenon_backend_capability_tests COMMAND xenon_backend_capability_tests)

    # Production-backend EDRAM ownership regression. This target is always
    # present so the common fixture is syntax-checked even when no native SDK
    # is installed; available renderer targets opt in below.
    add_executable(xenon_backend_ownership_integration_tests
      tests/graphics/backends/backend_ownership_integration.cpp)
    target_link_libraries(xenon_backend_ownership_integration_tests
      PRIVATE xenon_graphics)
    if(WIN32 AND TARGET xenon_graphics_d3d12)
      target_link_libraries(xenon_backend_ownership_integration_tests
        PRIVATE xenon_graphics_d3d12)
      target_compile_definitions(xenon_backend_ownership_integration_tests
        PRIVATE XENON_TEST_D3D12=1)
    endif()
    if(TARGET xenon_graphics_vulkan)
      target_link_libraries(xenon_backend_ownership_integration_tests
        PRIVATE xenon_graphics_vulkan)
      target_compile_definitions(xenon_backend_ownership_integration_tests
        PRIVATE XENON_TEST_VULKAN=1)
    endif()
    if(TARGET xenon_graphics_dxc)
      # The Vulkan/D3D12 backends this target links already depend on
      # xenon_graphics_dxc transitively and compile real shaders through it
      # at runtime (e.g. the depth-upload path exercised here) - missing this
      # call (unlike xenon_backend_capability_tests just above, which already
      # had it) left dxcompiler.dll unstaged next to this target's output,
      # making every shader-compiling assertion fail with "SPIR-V CodeGen not
      # available" despite a working DXC being found at configure time.
      xenon_stage_runtime_dependencies(xenon_backend_ownership_integration_tests)
    endif()
    xenon_enable_test_asserts(xenon_backend_ownership_integration_tests)
    add_test(NAME xenon_backend_ownership_integration_tests
      COMMAND xenon_backend_ownership_integration_tests)

    # Cross-backend canonical EDRAM handoff regression. Always syntax-check
    # the common fixture; the full suite runs when both native backends exist.
    add_executable(xenon_cross_backend_canonical_tests
      tests/graphics/backends/cross_backend_canonical.cpp)
    target_link_libraries(xenon_cross_backend_canonical_tests PRIVATE xenon_graphics)
    if(WIN32 AND TARGET xenon_graphics_d3d12)
      target_link_libraries(xenon_cross_backend_canonical_tests
        PRIVATE xenon_graphics_d3d12)
      target_compile_definitions(xenon_cross_backend_canonical_tests
        PRIVATE XENON_TEST_D3D12=1)
    endif()
    if(TARGET xenon_graphics_vulkan)
      target_link_libraries(xenon_cross_backend_canonical_tests
        PRIVATE xenon_graphics_vulkan)
      target_compile_definitions(xenon_cross_backend_canonical_tests
        PRIVATE XENON_TEST_VULKAN=1)
    endif()
    if(TARGET xenon_graphics_dxc)
      # Same missing-runtime-staging gap as xenon_backend_ownership_integration_tests
      # above - this target's canonical color/depth handoff paths compile real
      # shaders through the Vulkan backend's DXC dependency at runtime.
      xenon_stage_runtime_dependencies(xenon_cross_backend_canonical_tests)
    endif()
    xenon_enable_test_asserts(xenon_cross_backend_canonical_tests)
    add_test(NAME xenon_cross_backend_canonical_tests
      COMMAND xenon_cross_backend_canonical_tests)

    add_executable(xenon_shader_decoder_tests tests/graphics/xenos/shader_decoder.cpp)
    set_target_properties(xenon_shader_decoder_tests PROPERTIES
      OUTPUT_NAME xenon_shader_decoder_gpu09_tests)
    target_link_libraries(xenon_shader_decoder_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_shader_decoder_tests)
    add_test(NAME xenon_shader_decoder_tests COMMAND xenon_shader_decoder_tests)

    add_executable(xenon_depth_format_tests tests/graphics/xenos/depth_format.cpp)
    target_link_libraries(xenon_depth_format_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_depth_format_tests)
    add_test(NAME xenon_depth_format_tests COMMAND xenon_depth_format_tests)

    add_executable(xenon_shader_lowering_tests
      tests/graphics/xenos/shader_lowering.cpp)
    target_link_libraries(xenon_shader_lowering_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_shader_lowering_tests)
    add_test(NAME xenon_shader_lowering_tests COMMAND xenon_shader_lowering_tests)

    add_executable(xenon_resource_ir_tests tests/graphics/xenos/resource_ir.cpp)
    target_link_libraries(xenon_resource_ir_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_resource_ir_tests)
    add_test(NAME xenon_resource_ir_tests COMMAND xenon_resource_ir_tests)

    add_executable(xenon_resource_barrier_tests
      tests/graphics/xenos/resource_barrier.cpp)
    target_link_libraries(xenon_resource_barrier_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_resource_barrier_tests)
    add_test(NAME xenon_resource_barrier_tests COMMAND xenon_resource_barrier_tests)

    add_executable(xenon_texture_tests tests/graphics/xenos/texture.cpp)
    target_link_libraries(xenon_texture_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_texture_tests)
    add_test(NAME xenon_texture_tests COMMAND xenon_texture_tests)

    add_executable(xenon_presentation_tests tests/graphics/xenos/presentation.cpp)
    target_link_libraries(xenon_presentation_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_presentation_tests)
    add_test(NAME xenon_presentation_tests COMMAND xenon_presentation_tests)

    add_executable(xenon_edram_surface_tests
      tests/graphics/xenos/edram_surface.cpp)
    target_link_libraries(xenon_edram_surface_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_edram_surface_tests)
    add_test(NAME xenon_edram_surface_tests COMMAND xenon_edram_surface_tests)

    add_executable(xenon_edram_ownership_tests
      tests/graphics/xenos/edram_ownership.cpp)
    target_link_libraries(xenon_edram_ownership_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_edram_ownership_tests)
    add_test(NAME xenon_edram_ownership_tests COMMAND xenon_edram_ownership_tests)

    add_executable(xenon_primitive_processor_tests
      tests/graphics/xenos/primitive_processor.cpp)
    target_link_libraries(xenon_primitive_processor_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_primitive_processor_tests)
    add_test(NAME xenon_primitive_processor_tests COMMAND xenon_primitive_processor_tests)

    if(TARGET xenon_graphics_dxc)
      add_executable(xenon_shader_translation_tests
        tests/graphics/xenos/shader_translation.cpp)
      set_target_properties(xenon_shader_translation_tests PROPERTIES
        OUTPUT_NAME xenon_shader_translation_gpu07_tests)
      target_link_libraries(xenon_shader_translation_tests PRIVATE xenon_graphics_dxc)
      xenon_enable_test_asserts(xenon_shader_translation_tests)
      xenon_stage_runtime_dependencies(xenon_shader_translation_tests)
      add_test(NAME xenon_shader_translation_tests COMMAND xenon_shader_translation_tests)
    endif()

    add_executable(xenon_gpu_frontend_tests tests/graphics/xenos/gpu_frontend.cpp)
    set_target_properties(xenon_gpu_frontend_tests PROPERTIES
      OUTPUT_NAME xenon_gpu_frontend_gpu08_tests)
    target_link_libraries(xenon_gpu_frontend_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_gpu_frontend_tests)
    add_test(NAME xenon_gpu_frontend_tests COMMAND xenon_gpu_frontend_tests)
  endif()
endif()
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

if(XENON_BUILD_TESTS AND TARGET xenon_runtime_host)
  # LaunchConfig parsing/validation, isolated from process-spawning and Qt
  # (see launcher/tests/runtime/runtime_bridge_tests.cpp for the IPC-lifecycle
  # side of the same contract).
  add_executable(xenon_runtime_host_tests
    tests/runtime_host/launch_config_tests.cpp
    runtime_host/src/launch_config.cpp
    runtime_host/src/launch_config.hpp
  )
  target_include_directories(xenon_runtime_host_tests PRIVATE runtime_host/src)
  target_link_libraries(xenon_runtime_host_tests PRIVATE xenon_core)
  target_compile_features(xenon_runtime_host_tests PRIVATE cxx_std_20)
  xenon_enable_test_asserts(xenon_runtime_host_tests)
  add_test(NAME xenon_runtime_host_tests COMMAND xenon_runtime_host_tests)

  add_executable(xenon_runtime_content_source_tests
    tests/runtime_host/content_source_tests.cpp
    runtime_host/src/content_source.cpp
    runtime_host/src/content_source.hpp
  )
  target_include_directories(xenon_runtime_content_source_tests PRIVATE runtime_host/src)
  target_link_libraries(xenon_runtime_content_source_tests PRIVATE xenon_core)
  target_compile_features(xenon_runtime_content_source_tests PRIVATE cxx_std_20)
  xenon_enable_test_asserts(xenon_runtime_content_source_tests)
  add_test(NAME xenon_runtime_content_source_tests COMMAND xenon_runtime_content_source_tests)

  # Part 5.7 (Gracemeria readiness pass): structured launch-failure
  # categories in status.json - see tests/runtime_host/status_writer_tests.cpp.
  add_executable(xenon_status_writer_tests
    tests/runtime_host/status_writer_tests.cpp
    runtime_host/src/status_writer.cpp
    runtime_host/src/status_writer.hpp
    runtime_host/src/launch_config.cpp
    runtime_host/src/launch_config.hpp
  )
  target_include_directories(xenon_status_writer_tests PRIVATE runtime_host/src)
  target_link_libraries(xenon_status_writer_tests PRIVATE xenon_core)
  target_compile_features(xenon_status_writer_tests PRIVATE cxx_std_20)
  xenon_enable_test_asserts(xenon_status_writer_tests)
  add_test(NAME xenon_status_writer_tests COMMAND xenon_status_writer_tests)

  # Presentation window create()/destroy() rollback proof (item 6 of the
  # XenonSession closure pass) - see tests/runtime_host/presentation_host_tests.cpp.
  # This test's entire purpose is to exercise presentation_host.cpp for real,
  # so - matching the lesson from this project's Vulkan/D3D12 feature-flag
  # incident - a missing SDL2 must fail configuration loudly rather than
  # silently produce a test binary that can no longer validate anything.
  xenon_resolve_sdl2(XENON_PRESENTATION_HOST_TESTS_SDL2_TARGET)
  if(NOT XENON_PRESENTATION_HOST_TESTS_SDL2_TARGET)
    message(FATAL_ERROR
      "xenon_presentation_host_tests requires SDL2 (needed by presentation_host.cpp) - "
      "resolve an SDL2 dependency instead of silently skipping this test")
  endif()
  add_executable(xenon_presentation_host_tests
    tests/runtime_host/presentation_host_tests.cpp
    runtime_host/src/presentation_host.cpp
    runtime_host/src/presentation_host.hpp
  )
  target_include_directories(xenon_presentation_host_tests PRIVATE runtime_host/src)
  target_link_libraries(xenon_presentation_host_tests PRIVATE xenon_core ${XENON_PRESENTATION_HOST_TESTS_SDL2_TARGET})
  target_compile_features(xenon_presentation_host_tests PRIVATE cxx_std_20)
  xenon_enable_test_asserts(xenon_presentation_host_tests)
  add_test(NAME xenon_presentation_host_tests COMMAND xenon_presentation_host_tests)
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
