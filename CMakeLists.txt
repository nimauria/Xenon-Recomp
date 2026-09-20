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
  "Optional FFmpeg root exposing AV_CODEC_ID_XMAFRAMES; Windows x64 uses Xenon's bundled dependency by default")
option(XENON_ENABLE_INPUT "Enable input subsystem" ON)
option(XENON_INPUT_ENABLE_SDL2 "Enable SDL2 controller backend when SDL2 is available" ON)
option(XENON_INPUT_ENABLE_SDL3 "Enable SDL3 controller backend when SDL3 is available" ON)
option(XENON_INPUT_ENABLE_XINPUT "Enable native Windows XInput backend" ON)
option(XENON_ENABLE_NETWORK "Enable networking subsystem" OFF)
option(XENON_ENABLE_FILESYSTEM "Build the Xenon virtual filesystem subsystem" ON)
option(XENON_ENABLE_KERNEL "Build the Xenon host-side kernel object/I/O layer" ${XENON_ENABLE_FILESYSTEM})

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
  src/core/export_registry.cpp
  src/core/call_bridge.cpp
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
  src/cpu/codegen/executable_code_cache.cpp
  src/cpu/codegen/function_compiler.cpp
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
add_library(xenon_recomp STATIC
  src/recomp/driver.cpp
)
target_include_directories(xenon_recomp PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(xenon_recomp PUBLIC xenon_cpu xenon_xbox_kernel_io)
target_compile_features(xenon_recomp PUBLIC cxx_std_20)
xenon_enable_warnings(xenon_recomp)
add_library(Xenon::Recomp ALIAS xenon_recomp)

foreach(xenon_tool recomp-driver ppc-disasm ir-dump import-scanner module-inspector)
  add_executable(${xenon_tool} tools/recomp_tools.cpp)
  target_compile_definitions(${xenon_tool} PRIVATE XENON_TOOL_NAME="${xenon_tool}")
  target_link_libraries(${xenon_tool} PRIVATE xenon_recomp)
  xenon_enable_warnings(${xenon_tool})
endforeach()

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
    message(FATAL_ERROR
      "Audio V1 requires SDL2. Leave XENON_FETCH_MISSING_DEPS=ON to fetch Xenon's pinned SDL2 revision, install SDL2 development files, or configure -DXENON_ENABLE_AUDIO=OFF explicitly.")
  endif()
  if(NOT XENON_AUDIO_FFMPEG_TARGET)
    message(FATAL_ERROR
      "Audio V1 requires FFmpeg libavcodec/libavutil with AV_CODEC_ID_XMAFRAMES. Windows x64 checkouts include a vetted Xenon bundle; other platforms may provide XENON_AUDIO_FFMPEG_ROOT or a compatible system package.")
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
      "The selected FFmpeg does not expose AV_CODEC_ID_XMAFRAMES. Use Xenon's bundled Windows x64 dependency or provide a compatible build with XENON_AUDIO_FFMPEG_ROOT.")
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

  if(WIN32 AND XENON_ENABLE_DXC)
    # The Windows SDK DXC can emit DXIL only. Prefer the Vulkan SDK's matched
    # headers/import library/runtime because that build includes SPIR-V codegen.
    if(DEFINED ENV{VULKAN_SDK})
      find_path(XENON_DXC_INCLUDE_DIR dxc/dxcapi.h
        PATHS "$ENV{VULKAN_SDK}/Include" NO_DEFAULT_PATH)
      find_library(XENON_DXC_LIBRARY NAMES dxcompiler
        PATHS "$ENV{VULKAN_SDK}/Lib" NO_DEFAULT_PATH)
      find_file(XENON_DXC_RUNTIME NAMES dxcompiler.dll
        PATHS "$ENV{VULKAN_SDK}/Bin" NO_DEFAULT_PATH)
    endif()
    find_path(XENON_DXC_INCLUDE_DIR dxc/dxcapi.h)
    find_library(XENON_DXC_LIBRARY NAMES dxcompiler)
    find_file(XENON_DXC_RUNTIME NAMES dxcompiler.dll)
    if(XENON_DXC_INCLUDE_DIR AND XENON_DXC_LIBRARY)
      add_library(xenon_graphics_dxc STATIC
        src/graphics/dxc/shader_compiler.cpp)
      target_include_directories(xenon_graphics_dxc
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
        PRIVATE "${XENON_DXC_INCLUDE_DIR}")
      target_link_libraries(xenon_graphics_dxc PUBLIC xenon_graphics
        PRIVATE "${XENON_DXC_LIBRARY}")
      target_compile_features(xenon_graphics_dxc PUBLIC cxx_std_20)
      xenon_enable_warnings(xenon_graphics_dxc)
      add_library(Xenon::GraphicsDXC ALIAS xenon_graphics_dxc)
      message(STATUS "DXC shader compiler enabled: ${XENON_DXC_LIBRARY}")
    else()
      message(STATUS "DXC shader compiler disabled: dxcapi.h or dxcompiler library not found")
    endif()
  endif()

  if(XENON_ENABLE_VULKAN)
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
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
      target_link_libraries(xenon_graphics_vulkan PUBLIC xenon_graphics Vulkan::Vulkan)
      if(TARGET xenon_graphics_dxc)
        target_link_libraries(xenon_graphics_vulkan PRIVATE xenon_graphics_dxc)
        target_compile_definitions(xenon_graphics_vulkan PRIVATE XENON_HAS_DXC=1)
      endif()
      target_compile_features(xenon_graphics_vulkan PUBLIC cxx_std_20)
      xenon_enable_warnings(xenon_graphics_vulkan)
      add_library(Xenon::GraphicsVulkan ALIAS xenon_graphics_vulkan)
    else()
      message(STATUS "Vulkan runtime backend disabled: install the Vulkan SDK to build it")
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
if(TARGET xenon_graphics)
  target_link_libraries(xenon_core PUBLIC xenon_graphics)
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

  # Core session tests
  add_executable(xenon_session_tests tests/core/session_tests.cpp)
  target_link_libraries(xenon_session_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_session_tests)
  add_test(NAME xenon_session_tests COMMAND xenon_session_tests)

  if(TARGET xenon_audio)
    add_executable(xenon_audio_mixer_tests tests/audio/mixer_tests.cpp)
    target_link_libraries(xenon_audio_mixer_tests PRIVATE xenon_audio)
    xenon_enable_test_asserts(xenon_audio_mixer_tests)
    add_test(NAME xenon_audio_mixer_tests COMMAND xenon_audio_mixer_tests)

    add_executable(xenon_audio_xma_tests tests/audio/xma_tests.cpp)
    target_link_libraries(xenon_audio_xma_tests PRIVATE xenon_audio)
    xenon_enable_test_asserts(xenon_audio_xma_tests)
    add_test(NAME xenon_audio_xma_tests COMMAND xenon_audio_xma_tests)

    add_executable(xenon_audio_system_tests tests/audio/system_tests.cpp)
    target_link_libraries(xenon_audio_system_tests PRIVATE xenon_audio)
    xenon_enable_test_asserts(xenon_audio_system_tests)
    add_test(NAME xenon_audio_system_tests COMMAND xenon_audio_system_tests)

    # src/audio/exports.cpp is already compiled into xenon_core when Audio V1
    # is enabled; do not add it here again or static links gain duplicate
    # export-registration symbols.
    add_executable(xenon_audio_export_tests tests/audio/export_tests.cpp)
    target_link_libraries(xenon_audio_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_audio_export_tests)
    add_test(NAME xenon_audio_export_tests COMMAND xenon_audio_export_tests)
  endif()

  if(XENON_ENABLE_FILESYSTEM)
    add_executable(xenon_filesystem_tests tests/filesystem/filesystem_tests.cpp)
    target_link_libraries(xenon_filesystem_tests PRIVATE xenon_filesystem)
    xenon_enable_test_asserts(xenon_filesystem_tests)
    add_test(NAME xenon_filesystem_tests COMMAND xenon_filesystem_tests)
  endif()

  if(XENON_ENABLE_KERNEL)
    add_executable(xenon_kernel_io_tests tests/kernel/kernel_io_tests.cpp)
    target_link_libraries(xenon_kernel_io_tests PRIVATE xenon_kernel)
    xenon_enable_test_asserts(xenon_kernel_io_tests)
    add_test(NAME xenon_kernel_io_tests COMMAND xenon_kernel_io_tests)
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
  endif()

  add_executable(xenon_recomp_driver_tests tests/recomp/recomp_driver_tests.cpp)
  target_link_libraries(xenon_recomp_driver_tests PRIVATE xenon_recomp)
  target_compile_definitions(xenon_recomp_driver_tests PRIVATE
    XENON_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
  xenon_enable_test_asserts(xenon_recomp_driver_tests)
  add_test(NAME xenon_recomp_driver_tests COMMAND xenon_recomp_driver_tests)

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

  if(XENON_ENABLE_INPUT)
    add_executable(xenon_input_tests tests/input/input_tests.cpp)
    target_link_libraries(xenon_input_tests PRIVATE xenon_input)
    xenon_enable_test_asserts(xenon_input_tests)
    add_test(NAME xenon_input_tests COMMAND xenon_input_tests)

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
    if(TARGET xenon_graphics_dxc AND XENON_DXC_RUNTIME)
      target_link_libraries(xenon_backend_capability_tests PRIVATE xenon_graphics_dxc)
      target_compile_definitions(xenon_backend_capability_tests PRIVATE XENON_TEST_DXC=1)
      add_custom_command(TARGET xenon_backend_capability_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${XENON_DXC_RUNTIME}" "$<TARGET_FILE_DIR:xenon_backend_capability_tests>")
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
      if(XENON_DXC_RUNTIME)
        add_custom_command(TARGET xenon_shader_translation_tests POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${XENON_DXC_RUNTIME}" "$<TARGET_FILE_DIR:xenon_shader_translation_tests>")
      endif()
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
# and docs/RUNTIME_HOST.md.
add_subdirectory(runtime_host)

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
endif()

# -----------------------------------------------------------------------------
# Launcher UI. Kept isolated from the runtime so Qt does not leak into Xenon
# core, CPU, memory, graphics, or game-module interfaces.
if(XENON_BUILD_LAUNCHER)
  add_subdirectory(launcher)
endif()

# RuntimeBridge locates xenon_runtime_host next to the launcher executable at
# runtime (see docs/RUNTIME_HOST.md). Pin the host's output directory to the
# launcher's own so this holds for a plain dev build, not only after
# `cmake --install` places both under the same bin/. xenon_launcher's own
# output directory is left untouched (scripts such as
# launcher/scripts/build-launcher.ps1 expect it at build/<preset>/launcher/).
if(TARGET xenon_runtime_host AND TARGET xenon_launcher)
  set_target_properties(xenon_runtime_host PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<TARGET_FILE_DIR:xenon_launcher>")
endif()
# RuntimeBridge tests (xenon_runtime_host_fixture, xenon_runtime_bridge_tests)
# are declared inside launcher/CMakeLists.txt itself, in the same directory
# scope find_package(Qt6 ...) ran in - Qt6::Core etc. are imported targets
# only visible within that subdirectory's scope tree, not back up here.
