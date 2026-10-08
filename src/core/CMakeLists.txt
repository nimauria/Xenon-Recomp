# -----------------------------------------------------------------------------
# Core runtime
add_library(xenon_core STATIC
  runtime.cpp
  # XenonSession, by responsibility (see src/core/session/).
  session/session.cpp
  session/lifecycle/subsystem_init.cpp
  session/lifecycle/graphics_init.cpp
  session/lifecycle/input_init.cpp
  session/lifecycle/game_loading.cpp
  session/lifecycle/native_extension.cpp
  session/lifecycle/execution_control.cpp
  session/lifecycle/shutdown.cpp
  session/exports/export_registration.cpp
  session/exports/kernel_object_exports.cpp
  session/exports/kernel_variables.cpp
  session/threading/guest_process.cpp
  session/threading/thread_creation.cpp
  session/threading/thread_execution.cpp
  session/execution/guest_dispatch.cpp
  session/execution/runtime_services.cpp
  session/pumps/gpu_pump.cpp
  session/pumps/audio_callbacks.cpp
  session/diagnostics/adaptive_observations.cpp
  session/diagnostics/boot_progress.cpp
  session/diagnostics/fault_report.cpp
  session/diagnostics/memory_watch.cpp
  session/diagnostics/runtime_probes.cpp
  session/diagnostics/session_capability_report.cpp
  session/diagnostics/stop_report.cpp
  session/diagnostics/title_probes.cpp
  guest_thread_context.cpp
  export_registry.cpp
  call_bridge.cpp
  run_fingerprint.cpp
  capability_report.cpp
  import_classification.cpp
  boot_checkpoints.cpp
  export_trace.cpp
  guest_memory_watch.cpp
)
target_include_directories(xenon_core
  PUBLIC "${XENON_SOURCE_DIR}/include"
  PRIVATE "${XENON_SOURCE_DIR}/src")
target_compile_features(xenon_core PUBLIC cxx_std_20)
xenon_enable_warnings(xenon_core)
# xenon_logging is the only unconditional dependency. XenonSession uses every
# major subsystem, but those targets are optional and defined by later
# subdirectories, so the `if(TARGET ...)`-guarded links live at the end of
# src/CMakeLists.txt. Placed here, every check would see an undefined target
# and silently link nothing.
target_link_libraries(xenon_core PUBLIC xenon_logging)
add_library(Xenon::Core ALIAS xenon_core)
