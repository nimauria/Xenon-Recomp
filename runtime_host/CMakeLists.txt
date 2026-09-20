# Generic Xenon runtime/game-host process. This is the ONLY thing that ever
# owns a XenonSession and executes guest code. It is a plain console
# application with no Qt dependency (see docs/architecture/PROJECT_STRUCTURE.md
# - the launcher is a separate GUI outside the runtime dependency graph).
#
# The launcher spawns this process detached for each Play action and
# supervises it entirely through the file-based contract in
# docs/RUNTIME_HOST.md (a JSON launch configuration in, status.json/log.txt
# out, stop.signal to request a stop). This keeps a running game alive even
# if the launcher process exits or crashes.
add_executable(xenon_runtime_host
  src/main.cpp
  src/launch_config.cpp
  src/launch_config.hpp
  src/status_writer.cpp
  src/status_writer.hpp
)
target_link_libraries(xenon_runtime_host PRIVATE xenon_core)
target_include_directories(xenon_runtime_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_features(xenon_runtime_host PRIVATE cxx_std_20)
xenon_enable_warnings(xenon_runtime_host)

install(TARGETS xenon_runtime_host RUNTIME DESTINATION bin)
