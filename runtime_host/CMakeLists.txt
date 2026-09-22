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
xenon_enable_warnings(xenon_runtime_host)

# The game window belongs to this process (see presentation_host.hpp), reusing
# the same SDL2 dependency xenon_input/xenon_audio already resolve rather than
# adding a second window/UI toolkit.
xenon_resolve_sdl2(XENON_RUNTIME_HOST_SDL2_TARGET)
if(XENON_RUNTIME_HOST_SDL2_TARGET)
  target_link_libraries(xenon_runtime_host PRIVATE ${XENON_RUNTIME_HOST_SDL2_TARGET})
  target_compile_definitions(xenon_runtime_host PRIVATE XENON_HAS_PRESENTATION_HOST=1)
else()
  message(WARNING "xenon_runtime_host: SDL2 not resolved - game window/presentation will be disabled")
endif()

# Managed xenia-project FFmpeg is intentionally shared for release builds.
# Keep its loader-visible files next to the runtime host and include them in
# install/package output. This is a no-op when the committed static Windows
# developer bundle or a system dependency is selected.
xenon_stage_runtime_dependencies(xenon_runtime_host)

install(TARGETS xenon_runtime_host RUNTIME DESTINATION bin)
xenon_install_runtime_dependencies(bin "share/licenses/Xenon")
