# Generic Xenon runtime/game-host process. This is the ONLY thing that ever
# owns a XenonSession and executes guest code. It is a plain console
# application with no Qt dependency (see docs/architecture/PROJECT_STRUCTURE.md
# - the launcher is a separate GUI outside the runtime dependency graph).
#
# The launcher spawns this process detached for each Play action and
# supervises it entirely through the file-based contract in
# docs/runtime/RUNTIME_HOST.md (a JSON launch configuration in, status.json/log.txt
# out, stop.signal to request a stop). This keeps a running game alive even
# if the launcher process exits or crashes.
add_executable(xenon_runtime_host
  src/main.cpp
  src/host_setup.cpp
  src/host_setup.hpp
  src/content_source.cpp
  src/content_source.hpp
  src/crash_report.cpp
  src/crash_report.hpp
  src/launch_config.cpp
  src/launch_config.hpp
  src/status_writer.cpp
  src/status_writer.hpp
  src/presentation_host.cpp
  src/presentation_host.hpp
)
target_link_libraries(xenon_runtime_host PRIVATE xenon_core)
target_include_directories(xenon_runtime_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(xenon_runtime_host PRIVATE cxx_std_20)
target_compile_definitions(xenon_runtime_host PRIVATE
  XENON_HOST_PLATFORM_NAME="${XENON_HOST_PLATFORM}"
  XENON_HOST_ARCH_NAME="${XENON_HOST_ARCH}"
  XENON_RUNTIME_HAS_MEMORY=$<BOOL:${XENON_ENABLE_MEMORY}>)
xenon_enable_warnings(xenon_runtime_host)
if(MSVC)
  # Diagnostic-only: emit real debug info (PDB) and a MAP file so a live
  # debugger (cdb/windbg) can resolve this binary's own stack frames to
  # function names even in this Release build. Safe to remove once no
  # longer needed; does not affect optimization/codegen. /MAP without a path
  # writes xenon_runtime_host.map beside the executable, which also works
  # when the launcher (and its output directory) is not built.
  target_link_options(xenon_runtime_host PRIVATE /DEBUG /MAP)
endif()

# Host stack for every thread this process creates without an explicit size,
# which includes each guest thread's host thread. Recompiled guest functions
# keep their state in host frames several times the size of the PowerPC
# frames they replace, so recursion a guest stack holds can overflow the 1 MiB
# default: Ace Combat 6 died with STATUS_STACK_OVERFLOW starting the campaign
# opening and got past it with 64 MiB. Only address space is reserved; pages
# are committed as a stack actually grows. xenon_runtime_host_stack_tests
# checks the built executable keeps this.
set(XENON_RUNTIME_HOST_STACK_RESERVE 268435456 CACHE INTERNAL
    "Host stack reserved per xenon_runtime_host thread (256 MiB)")
if(MSVC)
  target_link_options(xenon_runtime_host PRIVATE /STACK:${XENON_RUNTIME_HOST_STACK_RESERVE})
elseif(WIN32)
  target_link_options(xenon_runtime_host PRIVATE
    -Wl,--stack,${XENON_RUNTIME_HOST_STACK_RESERVE})
endif()
# The game window belongs to this process (see presentation_host.hpp), reusing
# the same SDL3 dependency xenon_input/xenon_audio already resolve rather than
# adding a second window/UI toolkit.
xenon_resolve_sdl3(XENON_RUNTIME_HOST_SDL_TARGET)
if(XENON_RUNTIME_HOST_SDL_TARGET)
  target_link_libraries(xenon_runtime_host PRIVATE ${XENON_RUNTIME_HOST_SDL_TARGET})
  target_compile_definitions(xenon_runtime_host PRIVATE XENON_HAS_PRESENTATION_HOST=1)
else()
  message(WARNING "xenon_runtime_host: SDL3 not resolved - game window/presentation will be disabled")
endif()

# Managed xenia-project FFmpeg is intentionally shared for release builds.
# Keep its loader-visible files next to the runtime host and include them in
# install/package output. This is a no-op when the committed static Windows
# developer bundle or a system dependency is selected.
xenon_stage_runtime_dependencies(xenon_runtime_host)

install(TARGETS xenon_runtime_host RUNTIME DESTINATION bin)
xenon_install_runtime_dependencies(bin "share/licenses/Xenon")
