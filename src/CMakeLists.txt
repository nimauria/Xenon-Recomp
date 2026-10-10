# Xenon runtime libraries, one directory per subsystem. Order matters only
# for the `if(TARGET ...)` checks at the end, which must run after every
# optional subsystem has had the chance to define its target.

add_subdirectory(logging)
add_subdirectory(core)
add_subdirectory(xam)
add_subdirectory(network)
add_subdirectory(filesystem)
add_subdirectory(kernel)
add_subdirectory(cpu)
add_subdirectory(recomp)
add_subdirectory(memory)
add_subdirectory(audio)
add_subdirectory(input)
add_subdirectory(xbox)
add_subdirectory(graphics)

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
# dispatch() into the unified ExportRegistry (see
# src/core/session/exports/export_registration.cpp), so it
# needs the guest ABI bridge linked in whenever it was built.
if(TARGET xenon_input_guest)
  target_link_libraries(xenon_core PUBLIC xenon_input_guest)
endif()
if(TARGET xenon_graphics)
  target_link_libraries(xenon_core PUBLIC xenon_graphics)
endif()
# XenonSession::init_gpu() selects and constructs the real native backend
# directly (see src/core/session/lifecycle/graphics_init.cpp) rather than going through a separate
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
# src/xam/net/xam_net_exports.cpp calls the real host WSAStartup/WSACleanup for
# NetDll_WSAStartup/NetDll_WSACleanup (see that file for why this is a real
# implementation, not a stub).
if(WIN32)
  target_link_libraries(xenon_core PUBLIC ws2_32)
endif()
