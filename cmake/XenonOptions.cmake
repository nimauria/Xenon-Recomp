# Build options and their dependency rules. Included from the root
# CMakeLists.txt before any subsystem is configured.

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
