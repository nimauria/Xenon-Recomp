if(XENON_BUILD_TESTS)

  # Core session tests
  add_executable(xenon_session_tests session_tests.cpp)
  target_link_libraries(xenon_session_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_session_tests)
  add_test(NAME xenon_session_tests COMMAND xenon_session_tests)

  # Phase 0 (AC6 Runtime Readiness pass): shared telemetry/diagnostics
  # foundation - CapabilityReportBuilder aggregation, the Logger's
  # cheap-when-disabled gate, and RunFingerprint equality/serialization.
  add_executable(xenon_capability_report_tests capability_report_tests.cpp)
  target_link_libraries(xenon_capability_report_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_capability_report_tests)
  add_test(NAME xenon_capability_report_tests COMMAND xenon_capability_report_tests)

  add_executable(xenon_run_fingerprint_tests run_fingerprint_tests.cpp)
  target_link_libraries(xenon_run_fingerprint_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_run_fingerprint_tests)
  add_test(NAME xenon_run_fingerprint_tests COMMAND xenon_run_fingerprint_tests)

  add_executable(xenon_boot_checkpoint_tests boot_checkpoint_tests.cpp)
  target_link_libraries(xenon_boot_checkpoint_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_boot_checkpoint_tests)
  add_test(NAME xenon_boot_checkpoint_tests COMMAND xenon_boot_checkpoint_tests)

  add_executable(xenon_session_execution_tests
    session_execution_tests.cpp)
  target_link_libraries(xenon_session_execution_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_session_execution_tests)
  add_test(NAME xenon_session_execution_tests COMMAND xenon_session_execution_tests)

  # Phase 1/2 (AC6 Runtime Readiness pass): ExCreateThread, the shared
  # dispatch_guest_thread()/run_created_guest_thread() primitive, and the
  # external_call() real-calling-thread-id regression.
  add_executable(xenon_thread_creation_tests thread_creation_tests.cpp)
  target_link_libraries(xenon_thread_creation_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_thread_creation_tests)
  add_test(NAME xenon_thread_creation_tests COMMAND xenon_thread_creation_tests)

  # GPU pump thread (single combined ring-drain + vsync-present host thread
  # that drives the Xenos frontend/backend from a live XenonSession): startup/
  # shutdown lifecycle, ring-buffer drain integration, and front-buffer/
  # interrupt-callback vsync dispatch. See the file's header comment.
  add_executable(xenon_gpu_pump_tests gpu_pump_tests.cpp)
  target_link_libraries(xenon_gpu_pump_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_gpu_pump_tests)
  add_test(NAME xenon_gpu_pump_tests COMMAND xenon_gpu_pump_tests)

  # Phase 6 (AC6 Runtime Readiness pass): whole-XEX import capability audit
  # four-state classification (IMPLEMENTED/SAFE_STUB/PARTIAL/MISSING).
  add_executable(xenon_import_capability_report_tests import_capability_report_tests.cpp)
  target_link_libraries(xenon_import_capability_report_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_import_capability_report_tests)
  add_test(NAME xenon_import_capability_report_tests COMMAND xenon_import_capability_report_tests)

  # Proves the real "recompiled PPC -> import call -> CPU V2 -> external-call
  # bridge -> core::ExportRegistry -> active subsystem" path through the
  # actual production pipeline (XEX Loader V2, Recomp Driver, a compiled and
  # loaded native module, KernelProcess/KernelThread) - not a direct C++
  # shortcut. See tests/core/guest_export_abi_tests.cpp's header comment.
  if(TARGET xenon_audio)
    add_executable(xenon_guest_export_abi_tests guest_export_abi_tests.cpp)
    target_link_libraries(xenon_guest_export_abi_tests PRIVATE xenon_core xenon_recomp)
    target_compile_definitions(xenon_guest_export_abi_tests PRIVATE
      XENON_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
    xenon_enable_test_asserts(xenon_guest_export_abi_tests)
    xenon_test_uses_nested_cmake(xenon_guest_export_abi_tests)
    add_test(NAME xenon_guest_export_abi_tests COMMAND xenon_guest_export_abi_tests)
  endif()

  # Part 4 (Gracemeria readiness pass): proves the audio render-driver guest
  # callback runs with a real KernelProcess/KernelThread/KPCR/TLS identity
  # (XenonSession::start_audio_guest_thread()/invoke_audio_callback()) rather
  # than an isolated bare CpuState. See the file's header comment.
  if(TARGET xenon_audio)
    add_executable(xenon_audio_guest_callback_tests audio_guest_callback_tests.cpp)
    target_link_libraries(xenon_audio_guest_callback_tests PRIVATE xenon_core xenon_recomp)
    target_compile_definitions(xenon_audio_guest_callback_tests PRIVATE
      XENON_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
    xenon_enable_test_asserts(xenon_audio_guest_callback_tests)
    xenon_test_uses_nested_cmake(xenon_audio_guest_callback_tests)
    add_test(NAME xenon_audio_guest_callback_tests COMMAND xenon_audio_guest_callback_tests)
    set_tests_properties(
      xenon_thread_creation_tests
      xenon_guest_export_abi_tests
      xenon_audio_guest_callback_tests
      PROPERTIES ENVIRONMENT "SDL_AUDIODRIVER=dummy")
  endif()

  # Negative-path coverage for the same import/export dispatch boundary:
  # unknown library, known library/missing ordinal, a malformed native
  # import table entry, a call target that is neither compiled code nor a
  # known import, a missing/incompatible native module, an import-identity
  # (ordinal-collision) regression, and a KernelProcess/KernelMemory/guest-
  # stack rollback proof. See the file's header comment for why these use
  # the production import-resolution boundary directly rather than a second
  # full compiled-module build pipeline.
  add_executable(xenon_guest_import_negative_tests guest_import_negative_tests.cpp)
  target_link_libraries(xenon_guest_import_negative_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_guest_import_negative_tests)
  add_test(NAME xenon_guest_import_negative_tests COMMAND xenon_guest_import_negative_tests)

  # Minimal native "game module" used only by
  # tests/core/title_update_integration_tests.cpp to prove XenonSession's
  # effective-executable-revision compatibility gating through the real
  # LoadLibrary/dlopen native-extension path - see that fixture's own header
  # comment. It exports no real compiled code (never bound to a running
  # guest thread); it is not a game module and ships no game logic.
  add_library(xenon_title_update_test_module SHARED fixtures/title_update_test_module.cpp)
  target_link_libraries(xenon_title_update_test_module PRIVATE xenon_cpu)

  # Title Update Runtime Integration: proves a selected title update reaches
  # XenonSession::load_game()'s real xbox::apply_title_update() call (not
  # XEX Loader V2 called directly), that the effective (patched) image - not
  # the base image - is what gets mapped/run, that malformed/incompatible
  # updates fail explicitly with no partial guest-process state, and that a
  # native extension's declared effective-executable-revision compatibility
  # is actually enforced. See the file's header comment.
  add_executable(xenon_title_update_integration_tests title_update_integration_tests.cpp)
  target_link_libraries(xenon_title_update_integration_tests PRIVATE xenon_core)
  add_dependencies(xenon_title_update_integration_tests xenon_title_update_test_module)
  target_compile_definitions(xenon_title_update_integration_tests PRIVATE
    XENON_TITLE_UPDATE_TEST_MODULE_PATH="$<TARGET_FILE:xenon_title_update_test_module>")
  xenon_enable_test_asserts(xenon_title_update_integration_tests)
  add_test(NAME xenon_title_update_integration_tests COMMAND xenon_title_update_integration_tests)

  # Keep standalone regressions attached to a build target and CTest. The
  # source ownership audit checks this contract for every test main().
  xenon_add_standalone_test(xenon_export_trace_tests
    export_trace_tests.cpp xenon_core)
  xenon_add_standalone_test(xenon_guest_memory_watch_tests
    guest_memory_watch_tests.cpp xenon_core)
endif()
