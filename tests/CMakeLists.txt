# Tests and benchmarks. Executables and test modules stay at the build root
# so existing scripts and documentation paths keep working.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")

if(XENON_BUILD_BENCHMARKS AND XENON_ENABLE_MEMORY)
  add_executable(xenon_memory_v2_benchmarks
    memory/memory_benchmarks.cpp)
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
    recomp/recomp_analysis_benchmark.cpp)
  target_link_libraries(xenon_recomp_analysis_benchmark PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_recomp_analysis_benchmark)
endif()

if(XENON_BUILD_TESTS)
  function(xenon_add_standalone_test target source)
    add_executable(${target} "${source}")
    target_link_libraries(${target} PRIVATE ${ARGN})
    xenon_enable_test_asserts(${target})
    add_test(NAME ${target} COMMAND ${target})
  endfunction()

  find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND AND EXISTS "${XENON_SOURCE_DIR}/tests/deps/bootstrap_tests.py")
    add_test(NAME xenon_dependency_bootstrap_tests
      COMMAND "${Python3_EXECUTABLE}" "${XENON_SOURCE_DIR}/tests/deps/bootstrap_tests.py")
    set_tests_properties(xenon_dependency_bootstrap_tests PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
  if(Python3_Interpreter_FOUND AND EXISTS "${XENON_SOURCE_DIR}/tests/release/verify_install_tests.py")
    add_test(NAME xenon_release_verify_install_tests
      COMMAND "${Python3_EXECUTABLE}" "${XENON_SOURCE_DIR}/tests/release/verify_install_tests.py")
    set_tests_properties(xenon_release_verify_install_tests PROPERTIES
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()

  # Core session tests
  add_executable(xenon_session_tests core/session_tests.cpp)
  target_link_libraries(xenon_session_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_session_tests)
  add_test(NAME xenon_session_tests COMMAND xenon_session_tests)

  # Phase 0 (AC6 Runtime Readiness pass): shared telemetry/diagnostics
  # foundation - CapabilityReportBuilder aggregation, the Logger's
  # cheap-when-disabled gate, and RunFingerprint equality/serialization.
  add_executable(xenon_capability_report_tests core/capability_report_tests.cpp)
  target_link_libraries(xenon_capability_report_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_capability_report_tests)
  add_test(NAME xenon_capability_report_tests COMMAND xenon_capability_report_tests)

  add_executable(xenon_run_fingerprint_tests core/run_fingerprint_tests.cpp)
  target_link_libraries(xenon_run_fingerprint_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_run_fingerprint_tests)
  add_test(NAME xenon_run_fingerprint_tests COMMAND xenon_run_fingerprint_tests)

  add_executable(xenon_boot_checkpoint_tests core/boot_checkpoint_tests.cpp)
  target_link_libraries(xenon_boot_checkpoint_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_boot_checkpoint_tests)
  add_test(NAME xenon_boot_checkpoint_tests COMMAND xenon_boot_checkpoint_tests)

  add_executable(xenon_logger_stream_mutex_tests logging/logger_stream_mutex_tests.cpp)
  target_link_libraries(xenon_logger_stream_mutex_tests PRIVATE xenon_logging)
  xenon_enable_test_asserts(xenon_logger_stream_mutex_tests)
  add_test(NAME xenon_logger_stream_mutex_tests COMMAND xenon_logger_stream_mutex_tests)

  if(TARGET xenon_network)
    add_executable(xenon_network_tests network/network_tests.cpp)
    target_link_libraries(xenon_network_tests PRIVATE Xenon::Network)
    target_compile_definitions(xenon_network_tests PRIVATE
      XENON_NETWORK_FIXTURE_DIR="${XENON_SOURCE_DIR}/tests/network/fixtures")
    xenon_enable_test_asserts(xenon_network_tests)
    add_test(NAME xenon_network_tests COMMAND xenon_network_tests)
  endif()

  add_executable(xenon_session_execution_tests
    core/session_execution_tests.cpp)
  target_link_libraries(xenon_session_execution_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_session_execution_tests)
  add_test(NAME xenon_session_execution_tests COMMAND xenon_session_execution_tests)

  # Phase 1/2 (AC6 Runtime Readiness pass): ExCreateThread, the shared
  # dispatch_guest_thread()/run_created_guest_thread() primitive, and the
  # external_call() real-calling-thread-id regression.
  add_executable(xenon_thread_creation_tests core/thread_creation_tests.cpp)
  target_link_libraries(xenon_thread_creation_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_thread_creation_tests)
  add_test(NAME xenon_thread_creation_tests COMMAND xenon_thread_creation_tests)

  # GPU pump thread (single combined ring-drain + vsync-present host thread
  # that drives the Xenos frontend/backend from a live XenonSession): startup/
  # shutdown lifecycle, ring-buffer drain integration, and front-buffer/
  # interrupt-callback vsync dispatch. See the file's header comment.
  add_executable(xenon_gpu_pump_tests core/gpu_pump_tests.cpp)
  target_link_libraries(xenon_gpu_pump_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_gpu_pump_tests)
  add_test(NAME xenon_gpu_pump_tests COMMAND xenon_gpu_pump_tests)

  # Phase 6 (AC6 Runtime Readiness pass): whole-XEX import capability audit
  # four-state classification (IMPLEMENTED/SAFE_STUB/PARTIAL/MISSING).
  add_executable(xenon_import_capability_report_tests core/import_capability_report_tests.cpp)
  target_link_libraries(xenon_import_capability_report_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_import_capability_report_tests)
  add_test(NAME xenon_import_capability_report_tests COMMAND xenon_import_capability_report_tests)

  if(TARGET xenon_audio)
    add_executable(xenon_audio_mixer_tests audio/mixer_tests.cpp)
    target_link_libraries(xenon_audio_mixer_tests PRIVATE xenon_audio)
    xenon_enable_test_asserts(xenon_audio_mixer_tests)
    xenon_stage_runtime_dependencies(xenon_audio_mixer_tests)
    add_test(NAME xenon_audio_mixer_tests COMMAND xenon_audio_mixer_tests)

    add_executable(xenon_audio_xma_tests audio/xma_tests.cpp)
    target_link_libraries(xenon_audio_xma_tests PRIVATE xenon_audio)
    xenon_enable_test_asserts(xenon_audio_xma_tests)
    xenon_stage_runtime_dependencies(xenon_audio_xma_tests)
    add_test(NAME xenon_audio_xma_tests COMMAND xenon_audio_xma_tests)

    add_executable(xenon_audio_system_tests audio/system_tests.cpp)
    target_link_libraries(xenon_audio_system_tests PRIVATE xenon_audio)
    xenon_enable_test_asserts(xenon_audio_system_tests)
    xenon_stage_runtime_dependencies(xenon_audio_system_tests)
    add_test(NAME xenon_audio_system_tests COMMAND xenon_audio_system_tests)

    # src/audio/exports.cpp is already compiled into xenon_core when Audio V1
    # is enabled; do not add it here again or static links gain duplicate
    # export-registration symbols.
    add_executable(xenon_audio_export_tests audio/export_tests.cpp)
    target_link_libraries(xenon_audio_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_audio_export_tests)
    xenon_stage_runtime_dependencies(xenon_audio_export_tests)
    add_test(NAME xenon_audio_export_tests COMMAND xenon_audio_export_tests)
  endif()

  if(XENON_ENABLE_FILESYSTEM)
    add_executable(xenon_filesystem_tests filesystem/filesystem_tests.cpp)
    target_link_libraries(xenon_filesystem_tests PRIVATE xenon_filesystem)
    xenon_enable_test_asserts(xenon_filesystem_tests)
    add_test(NAME xenon_filesystem_tests COMMAND xenon_filesystem_tests)
  endif()

  if(XENON_ENABLE_KERNEL)
    if(XENON_ENABLE_MEMORY)
      add_executable(xenon_guest_heap_tests kernel/guest_heap_tests.cpp)
      target_link_libraries(xenon_guest_heap_tests PRIVATE xenon_kernel)
      xenon_enable_test_asserts(xenon_guest_heap_tests)
      add_test(NAME xenon_guest_heap_tests COMMAND xenon_guest_heap_tests)
    endif()

    add_executable(xenon_kernel_io_tests kernel/kernel_io_tests.cpp)
    target_link_libraries(xenon_kernel_io_tests PRIVATE xenon_kernel)
    xenon_enable_test_asserts(xenon_kernel_io_tests)
    add_test(NAME xenon_kernel_io_tests COMMAND xenon_kernel_io_tests)

    # Phase 3 (AC6 Runtime Readiness pass): mutant owner-id-1 collision and
    # wait_on_object() mutant thread-identity regressions.
    add_executable(xenon_kernel_synchronization_tests kernel/synchronization_tests.cpp)
    target_link_libraries(xenon_kernel_synchronization_tests PRIVATE xenon_kernel)
    xenon_enable_test_asserts(xenon_kernel_synchronization_tests)
    add_test(NAME xenon_kernel_synchronization_tests COMMAND xenon_kernel_synchronization_tests)

    # Phase 3 (AC6 Runtime Readiness pass): real timer-dispatch thread -
    # a timer with due_time > 0 now actually fires, periodic re-arm works,
    # cancel prevents firing, and teardown does not hang.
    add_executable(xenon_kernel_timer_dispatch_tests kernel/timer_dispatch_tests.cpp)
    target_link_libraries(xenon_kernel_timer_dispatch_tests PRIVATE xenon_kernel)
    xenon_enable_test_asserts(xenon_kernel_timer_dispatch_tests)
    add_test(NAME xenon_kernel_timer_dispatch_tests COMMAND xenon_kernel_timer_dispatch_tests)

    # Phase 5 (AC6 Runtime Readiness pass): ExceptionDispatcher::fault_to_exception()
    # Execute-fault/ExceptionInformation[0] regression.
    add_executable(xenon_kernel_exception_tests kernel/exception_tests.cpp)
    target_link_libraries(xenon_kernel_exception_tests PRIVATE xenon_kernel)
    xenon_enable_test_asserts(xenon_kernel_exception_tests)
    add_test(NAME xenon_kernel_exception_tests COMMAND xenon_kernel_exception_tests)

    # Phase 2 (AC6 Runtime Readiness pass): join(timeout) real-timeout,
    # terminate()-vs-natural-return exit-code race, and CREATE_SUSPENDED
    # regressions.
    add_executable(xenon_kernel_thread_lifecycle_tests kernel/thread_lifecycle_tests.cpp)
    target_link_libraries(xenon_kernel_thread_lifecycle_tests PRIVATE xenon_kernel)
    xenon_enable_test_asserts(xenon_kernel_thread_lifecycle_tests)
    add_test(NAME xenon_kernel_thread_lifecycle_tests COMMAND xenon_kernel_thread_lifecycle_tests)
  endif()

  if(TARGET xenon_xbox_kernel_io)
    add_executable(xenon_xbox_guest_io_tests kernel/xbox_guest_io_tests.cpp)
    target_link_libraries(xenon_xbox_guest_io_tests PRIVATE xenon_xbox_kernel_io)
    xenon_enable_test_asserts(xenon_xbox_guest_io_tests)
    add_test(NAME xenon_xbox_guest_io_tests COMMAND xenon_xbox_guest_io_tests)
  endif()

  if(TARGET xenon_xbox_kernel_io)
    add_executable(xenon_xbox_import_tests xbox/xbox_import_tests.cpp)
    target_link_libraries(xenon_xbox_import_tests PRIVATE xenon_xbox_kernel_io)
    xenon_enable_test_asserts(xenon_xbox_import_tests)
    add_test(NAME xenon_xbox_import_tests COMMAND xenon_xbox_import_tests)

    add_executable(xenon_xex_loader_tests xbox/xex_loader_tests.cpp)
    target_link_libraries(xenon_xex_loader_tests PRIVATE xenon_xbox_kernel_io)
    xenon_enable_test_asserts(xenon_xex_loader_tests)
    add_test(NAME xenon_xex_loader_tests COMMAND xenon_xex_loader_tests)

    add_executable(xenon_xex_crypto_tests xbox/xex_crypto_tests.cpp)
    target_link_libraries(xenon_xex_crypto_tests PRIVATE xenon_xbox_kernel_io)
    xenon_enable_test_asserts(xenon_xex_crypto_tests)
    add_test(NAME xenon_xex_crypto_tests COMMAND xenon_xex_crypto_tests)

    add_executable(xenon_xex_lzx_tests xbox/xex_lzx_tests.cpp)
    target_link_libraries(xenon_xex_lzx_tests PRIVATE xenon_xbox_kernel_io)
    xenon_enable_test_asserts(xenon_xex_lzx_tests)
    add_test(NAME xenon_xex_lzx_tests COMMAND xenon_xex_lzx_tests)

    add_executable(xenon_rtl_tests xbox/rtl_tests.cpp)
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
    add_executable(xenon_xboxkrnl_time_export_tests xbox/time_export_tests.cpp)
    target_link_libraries(xenon_xboxkrnl_time_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_xboxkrnl_time_export_tests)
    add_test(NAME xenon_xboxkrnl_time_export_tests COMMAND xenon_xboxkrnl_time_export_tests)
  endif()

  # xboxkrnl guest memory-management exports (KeFlushUserModeTb,
  # NtAllocateVirtualMemory) - the first real kernel exports Ace Combat 6's
  # actual boot path calls that had no ExportRegistry registration at all.
  # See xenon_xboxkrnl_time_export_tests above for why this links xenon_core
  # rather than xenon_xbox_kernel_io alone.
  if(TARGET xenon_core)
    add_executable(xenon_xboxkrnl_memory_export_tests xbox/memory_export_tests.cpp)
    target_link_libraries(xenon_xboxkrnl_memory_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_xboxkrnl_memory_export_tests)
    add_test(NAME xenon_xboxkrnl_memory_export_tests COMMAND xenon_xboxkrnl_memory_export_tests)
  endif()

  # xboxkrnl process-type exports (KeGetCurrentProcessType,
  # KeSetCurrentProcessType) - the next real export Ace Combat 6's boot path
  # calls once the memory-management exports above were fixed.
  if(TARGET xenon_core)
    add_executable(xenon_xboxkrnl_process_export_tests xbox/process_export_tests.cpp)
    target_link_libraries(xenon_xboxkrnl_process_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_xboxkrnl_process_export_tests)
    add_test(NAME xenon_xboxkrnl_process_export_tests COMMAND xenon_xboxkrnl_process_export_tests)
  endif()

  # xboxkrnl Rtl*CriticalSection exports - the next real export Ace Combat
  # 6's boot path calls once the process-type exports above were fixed.
  # Includes a real multi-host-thread mutual-exclusion test, not just
  # single-threaded field bookkeeping.
  if(TARGET xenon_core)
    add_executable(xenon_rtl_critical_section_export_tests
      xbox/rtl_critical_section_export_tests.cpp)
    target_link_libraries(xenon_rtl_critical_section_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_rtl_critical_section_export_tests)
    add_test(NAME xenon_rtl_critical_section_export_tests
             COMMAND xenon_rtl_critical_section_export_tests)
  endif()

  # xboxkrnl XexCheckExecutablePrivilege - the next real export Ace Combat
  # 6's boot path calls once the gpr[12]-vs-gpr[0] save/restore-helper
  # LR-source bug was fixed.
  if(TARGET xenon_core)
    add_executable(xenon_xex_module_export_tests xbox/xex_module_export_tests.cpp)
    target_link_libraries(xenon_xex_module_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_xex_module_export_tests)
    add_test(NAME xenon_xex_module_export_tests COMMAND xenon_xex_module_export_tests)
  endif()

  # xboxkrnl KeTls* exports - the next real export Ace Combat 6's boot path
  # calls once XexCheckExecutablePrivilege above was fixed.
  if(TARGET xenon_core)
    add_executable(xenon_tls_export_tests xbox/tls_export_tests.cpp)
    target_link_libraries(xenon_tls_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_tls_export_tests)
    add_test(NAME xenon_tls_export_tests COMMAND xenon_tls_export_tests)
  endif()

  # xboxkrnl Vd* Xenos GPU control-plane exports - the missing kernel ABI
  # surface between the fully-implemented Xenos GPU frontend
  # (src/graphics/xenos/) and a live XenonSession: VdInitializeRingBuffer/
  # VdEnableRingBufferRPtrWriteBack/VdSetGraphicsInterruptCallback/VdSwap hand
  # the guest's GPU command ring buffer, front buffer and vblank interrupt
  # callback to KernelProcess's Gpu*State for a separate GPU pump thread
  # workstream to consume.
  if(TARGET xenon_core)
    add_executable(xenon_video_export_tests xbox/video_export_tests.cpp)
    target_link_libraries(xenon_video_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_video_export_tests)
    add_test(NAME xenon_video_export_tests COMMAND xenon_video_export_tests)
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
    add_executable(xenon_xboxkrnl_sync_export_tests xbox/sync_export_tests.cpp)
    target_link_libraries(xenon_xboxkrnl_sync_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_xboxkrnl_sync_export_tests)
    add_test(NAME xenon_xboxkrnl_sync_export_tests COMMAND xenon_xboxkrnl_sync_export_tests)

    # Phase 3 (AC6 Runtime Readiness pass): xboxkrnl Ke* raw-guest-memory
    # dispatcher-object exports (X_DISPATCH_HEADER resolution, lazy
    # host-object stashing, Event/Semaphore semantics).
    add_executable(xenon_xboxkrnl_ke_sync_export_tests xbox/ke_sync_export_tests.cpp)
    target_link_libraries(xenon_xboxkrnl_ke_sync_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_xboxkrnl_ke_sync_export_tests)
    add_test(NAME xenon_xboxkrnl_ke_sync_export_tests COMMAND xenon_xboxkrnl_ke_sync_export_tests)

    add_executable(xenon_ke_irql_export_tests xbox/ke_irql_export_tests.cpp)
    target_link_libraries(xenon_ke_irql_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_ke_irql_export_tests)
    add_test(NAME xenon_ke_irql_export_tests COMMAND xenon_ke_irql_export_tests)

    add_executable(xenon_rtl_string_memory_export_tests
      xbox/rtl_string_memory_export_tests.cpp)
    target_link_libraries(xenon_rtl_string_memory_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_rtl_string_memory_export_tests)
    add_test(NAME xenon_rtl_string_memory_export_tests
             COMMAND xenon_rtl_string_memory_export_tests)

    add_executable(xenon_misc_export_tests xbox/misc_export_tests.cpp)
    target_link_libraries(xenon_misc_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_misc_export_tests)
    add_test(NAME xenon_misc_export_tests COMMAND xenon_misc_export_tests)
  endif()

  add_executable(xenon_recomp_driver_tests recomp/recomp_driver_tests.cpp)
  target_link_libraries(xenon_recomp_driver_tests PRIVATE xenon_recomp)
  target_compile_definitions(xenon_recomp_driver_tests PRIVATE
    XENON_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
  xenon_enable_test_asserts(xenon_recomp_driver_tests)
  add_test(NAME xenon_recomp_driver_tests COMMAND xenon_recomp_driver_tests)

  # Generic worker pool (Part 3 of the Recomp Analysis V2 pass): correctness
  # of parallel_for() itself (every index run exactly once, result ordering
  # independent of completion order, exception propagation, no races under
  # repeated stress runs), independent of the Recomp Driver that consumes it.
  add_executable(xenon_worker_pool_tests recomp/worker_pool_tests.cpp)
  target_link_libraries(xenon_worker_pool_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_worker_pool_tests)
  add_test(NAME xenon_worker_pool_tests COMMAND xenon_worker_pool_tests)

  # Parallel analysis determinism (Part 4/21): jobs=1 vs jobs=N produce
  # byte-identical deterministic AnalysisReport content on a synthetic
  # multi-function fixture, and discovery waves converge correctly.
  add_executable(xenon_recomp_parallel_analysis_tests recomp/parallel_analysis_tests.cpp)
  target_link_libraries(xenon_recomp_parallel_analysis_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_recomp_parallel_analysis_tests)
  add_test(NAME xenon_recomp_parallel_analysis_tests COMMAND xenon_recomp_parallel_analysis_tests)

  # Recomp Analysis V3 (discovery-quality pass): provenance/evidence
  # combination, confidence, generic TLS-callback seeding, bounded CTR
  # dataflow resolution, invalid-vs-unsupported PPC classification, the
  # conservative prologue heuristic, validated tail calls, and
  # switch/jump-table bounds-check gating.
  add_executable(xenon_discovery_quality_tests recomp/discovery_quality_tests.cpp)
  target_link_libraries(xenon_discovery_quality_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_discovery_quality_tests)
  add_test(NAME xenon_discovery_quality_tests COMMAND xenon_discovery_quality_tests)

  # Gen 6 PPC value/indirect-flow analysis: bounded abstract-value lattice,
  # cheap block-local propagation, read-only pointer/vtable folding, and the
  # selective bounded backward-slice resolver.
  add_executable(xenon_ppc_value_analysis_tests recomp/ppc_value_analysis_tests.cpp)
  target_link_libraries(xenon_ppc_value_analysis_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_ppc_value_analysis_tests)
  add_test(NAME xenon_ppc_value_analysis_tests COMMAND xenon_ppc_value_analysis_tests)

  # Gen 9 universal knowledge base: versioned normalized fingerprints,
  # cross-revision matching, confidence gates, JSONL persistence and replay.
  find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
  add_test(NAME xenon_native_cache_tests COMMAND "${Python3_EXECUTABLE}" "${XENON_SOURCE_DIR}/tests/recomp/native_cache_tests.py")
  set_tests_properties(xenon_native_cache_tests PROPERTIES ENVIRONMENT "XENON_TEST_CXX=${CMAKE_CXX_COMPILER}")
  add_executable(xenon_compilation_graph_tests recomp/compilation_graph_tests.cpp)
  target_link_libraries(xenon_compilation_graph_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_compilation_graph_tests)
  add_test(NAME xenon_compilation_graph_tests COMMAND xenon_compilation_graph_tests)

  # Gen 11 autonomous game intake: multi-executable discovery over a
  # directory, a loose .xex file and a synthetic disc image, plus the Game
  # Compilation Graph's JSON shape.
  add_executable(xenon_game_intake_tests recomp/game_intake_tests.cpp)
  target_link_libraries(xenon_game_intake_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_game_intake_tests)
  add_test(NAME xenon_game_intake_tests COMMAND xenon_game_intake_tests)

  add_executable(xenon_knowledge_base_tests recomp/knowledge_base_tests.cpp)
  target_link_libraries(xenon_knowledge_base_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_knowledge_base_tests)
  add_test(NAME xenon_knowledge_base_tests COMMAND xenon_knowledge_base_tests)

  # Prepared native-module artifact cache (automatic game preparation
  # pipeline): digest/invalidation semantics and real stage->validate->
  # atomic-promote/rollback behaviour, including a real compiled-and-loaded
  # shared library check (not a stubbed file-existence check).
  add_executable(xenon_artifact_cache_tests recomp/artifact_cache_tests.cpp)
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
  add_executable(xenon_prepare_worker_tests recomp/xenon_prepare_worker_tests.cpp)
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
  add_executable(xenon_analysis_schema_tests recomp/analysis_schema_tests.cpp)
  target_link_libraries(xenon_analysis_schema_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_analysis_schema_tests)
  add_test(NAME xenon_analysis_schema_tests COMMAND xenon_analysis_schema_tests)

  # Analysis Hint Schema V2 consumption: proves the Recomp Driver actually
  # uses chunks/switch tables/known indirect calls/native replacements/
  # regions during real analysis, not merely parses/stores them (Part 1.11).
  add_executable(xenon_heap_native_replacement_tests
    recomp/heap_native_replacement_tests.cpp)
  target_link_libraries(xenon_heap_native_replacement_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_heap_native_replacement_tests)
  add_test(NAME xenon_heap_native_replacement_tests
    COMMAND xenon_heap_native_replacement_tests)

  add_executable(xenon_analysis_v2_consumption_tests recomp/analysis_v2_consumption_tests.cpp)
  target_link_libraries(xenon_analysis_v2_consumption_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_analysis_v2_consumption_tests)
  add_test(NAME xenon_analysis_v2_consumption_tests COMMAND xenon_analysis_v2_consumption_tests)

  # Generated-code deduplication / shard ownership fix: proves the actual
  # production bug (byte-identical functions colliding in the codegen
  # content-addressed cache, producing MSVC C2084 duplicate-body errors on a
  # real title) stays fixed, plus the FunctionChunk/independent-FunctionHint
  # mutual-exclusion fix, parallel codegen determinism, and the
  # fail-fast symbol/address uniqueness validation added alongside it.
  add_executable(xenon_codegen_ownership_tests recomp/codegen_ownership_tests.cpp)
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
  add_executable(xenon_registry_numeric_format_tests recomp/registry_numeric_format_tests.cpp)
  target_link_libraries(xenon_registry_numeric_format_tests PRIVATE xenon_recomp)
  target_compile_definitions(xenon_registry_numeric_format_tests PRIVATE
    XENON_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
  xenon_enable_test_asserts(xenon_registry_numeric_format_tests)
  add_test(NAME xenon_registry_numeric_format_tests COMMAND xenon_registry_numeric_format_tests)

  # Import-thunk static-analysis misclassification fix: an XEX-native
  # import record's callable guest_thunk address must never be decoded as
  # PPC or shadow the runtime's existing import dispatch in the generated
  # lookup_compiled() table - see analyze_function_candidate()'s
  # find_callable_import_thunk() short-circuit and the real Ace Combat 6
  # 0x823D014C repro in the test file's own header comment.
  add_executable(xenon_import_thunk_analysis_tests recomp/import_thunk_analysis_tests.cpp)
  target_link_libraries(xenon_import_thunk_analysis_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_import_thunk_analysis_tests)
  add_test(NAME xenon_import_thunk_analysis_tests COMMAND xenon_import_thunk_analysis_tests)

  # Tail-call-only shared-block discovery fix: a terminal, non-linked branch
  # to a genuine, valid PPC block with no `bl` caller anywhere and no
  # recognized function prologue must still be discovered/compiled, not left
  # as an entry-integrity-breaking hole - see
  # analyze_function_candidate()'s decodes_to_valid_terminated_block() and
  # the real Ace Combat 6 0x8224c9a0 -> 0x8224c458 repro in the test file's
  # own header comment.
  add_executable(xenon_tail_call_gap_discovery_tests recomp/tail_call_gap_discovery_tests.cpp)
  target_link_libraries(xenon_tail_call_gap_discovery_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_tail_call_gap_discovery_tests)
  add_test(NAME xenon_tail_call_gap_discovery_tests COMMAND xenon_tail_call_gap_discovery_tests)

  # ModuleHintProvider production integration (Part 2/7): a synthetic
  # two-revision module package, correct/incorrect revision selection, and
  # proof the provider is actually consumed by load_and_analyze().
  add_executable(xenon_module_hint_provider_tests recomp/module_hint_provider_tests.cpp)
  target_link_libraries(xenon_module_hint_provider_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_module_hint_provider_tests)
  add_test(NAME xenon_module_hint_provider_tests COMMAND xenon_module_hint_provider_tests)

  # Proves the real "recompiled PPC -> import call -> CPU V2 -> external-call
  # bridge -> core::ExportRegistry -> active subsystem" path through the
  # actual production pipeline (XEX Loader V2, Recomp Driver, a compiled and
  # loaded native module, KernelProcess/KernelThread) - not a direct C++
  # shortcut. See tests/core/guest_export_abi_tests.cpp's header comment.
  if(TARGET xenon_audio)
    add_executable(xenon_guest_export_abi_tests core/guest_export_abi_tests.cpp)
    target_link_libraries(xenon_guest_export_abi_tests PRIVATE xenon_core xenon_recomp)
    target_compile_definitions(xenon_guest_export_abi_tests PRIVATE
      XENON_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
    xenon_enable_test_asserts(xenon_guest_export_abi_tests)
    add_test(NAME xenon_guest_export_abi_tests COMMAND xenon_guest_export_abi_tests)
  endif()

  # Part 4 (Gracemeria readiness pass): proves the audio render-driver guest
  # callback runs with a real KernelProcess/KernelThread/KPCR/TLS identity
  # (XenonSession::start_audio_guest_thread()/invoke_audio_callback()) rather
  # than an isolated bare CpuState. See the file's header comment.
  if(TARGET xenon_audio)
    add_executable(xenon_audio_guest_callback_tests core/audio_guest_callback_tests.cpp)
    target_link_libraries(xenon_audio_guest_callback_tests PRIVATE xenon_core xenon_recomp)
    target_compile_definitions(xenon_audio_guest_callback_tests PRIVATE
      XENON_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
    xenon_enable_test_asserts(xenon_audio_guest_callback_tests)
    add_test(NAME xenon_audio_guest_callback_tests COMMAND xenon_audio_guest_callback_tests)

    # These exercise the production SDL audio path without requiring a
    # physical audio device on headless developer and CI machines.
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
  add_executable(xenon_guest_import_negative_tests core/guest_import_negative_tests.cpp)
  target_link_libraries(xenon_guest_import_negative_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_guest_import_negative_tests)
  add_test(NAME xenon_guest_import_negative_tests COMMAND xenon_guest_import_negative_tests)

  # Minimal native "game module" used only by
  # tests/core/title_update_integration_tests.cpp to prove XenonSession's
  # effective-executable-revision compatibility gating through the real
  # LoadLibrary/dlopen native-extension path - see that fixture's own header
  # comment. It exports no real compiled code (never bound to a running
  # guest thread); it is not a game module and ships no game logic.
  add_library(xenon_title_update_test_module SHARED core/fixtures/title_update_test_module.cpp)
  target_link_libraries(xenon_title_update_test_module PRIVATE xenon_cpu)

  # Title Update Runtime Integration: proves a selected title update reaches
  # XenonSession::load_game()'s real xbox::apply_title_update() call (not
  # XEX Loader V2 called directly), that the effective (patched) image - not
  # the base image - is what gets mapped/run, that malformed/incompatible
  # updates fail explicitly with no partial guest-process state, and that a
  # native extension's declared effective-executable-revision compatibility
  # is actually enforced. See the file's header comment.
  add_executable(xenon_title_update_integration_tests core/title_update_integration_tests.cpp)
  target_link_libraries(xenon_title_update_integration_tests PRIVATE xenon_core)
  add_dependencies(xenon_title_update_integration_tests xenon_title_update_test_module)
  target_compile_definitions(xenon_title_update_integration_tests PRIVATE
    XENON_TITLE_UPDATE_TEST_MODULE_PATH="$<TARGET_FILE:xenon_title_update_test_module>")
  xenon_enable_test_asserts(xenon_title_update_integration_tests)
  add_test(NAME xenon_title_update_integration_tests COMMAND xenon_title_update_integration_tests)

  add_executable(xenon_cpu_tests cpu/ppc/cpu_tests.cpp)
  target_link_libraries(xenon_cpu_tests PRIVATE xenon_cpu)
  target_include_directories(xenon_cpu_tests PRIVATE "${XENON_SOURCE_DIR}/src/cpu/ppc/decoder")
  xenon_enable_test_asserts(xenon_cpu_tests)
  add_test(NAME xenon_cpu_tests COMMAND xenon_cpu_tests)

  add_executable(xenon_cpu_v2_cfg cpu/ir/cpu_v2_cfg.cpp)
  target_link_libraries(xenon_cpu_v2_cfg PRIVATE xenon_cpu)
  xenon_enable_test_asserts(xenon_cpu_v2_cfg)
  add_test(NAME xenon_cpu_v2_cfg COMMAND xenon_cpu_v2_cfg)

  add_executable(xenon_cpu_v2_ir cpu/ir/cpu_v2_ir.cpp)
  target_link_libraries(xenon_cpu_v2_ir PRIVATE xenon_cpu)
  xenon_enable_test_asserts(xenon_cpu_v2_ir)
  add_test(NAME xenon_cpu_v2_ir COMMAND xenon_cpu_v2_ir)

  add_executable(xenon_cpu_v2_state_promotion cpu/optimizer/cpu_v2_state_promotion.cpp)
  target_link_libraries(xenon_cpu_v2_state_promotion PRIVATE xenon_cpu)
  xenon_enable_test_asserts(xenon_cpu_v2_state_promotion)
  add_test(NAME xenon_cpu_v2_state_promotion COMMAND xenon_cpu_v2_state_promotion)

  # Generate and compile one function for every canonical instruction.
  add_executable(xenon_cpu_codegen_all cpu/codegen/codegen_all.cpp)
  target_link_libraries(xenon_cpu_codegen_all PRIVATE xenon_cpu)
  set(XENON_GENERATED_ALL "${CMAKE_CURRENT_BINARY_DIR}/generated_all_opcodes.cpp")
  add_custom_command(
    OUTPUT "${XENON_GENERATED_ALL}"
    COMMAND xenon_cpu_codegen_all "${XENON_GENERATED_ALL}"
    DEPENDS xenon_cpu_codegen_all
    VERBATIM)
  add_library(xenon_cpu_generated_compile OBJECT "${XENON_GENERATED_ALL}")
  target_include_directories(xenon_cpu_generated_compile PRIVATE "${XENON_SOURCE_DIR}/include")
  target_compile_features(xenon_cpu_generated_compile PRIVATE cxx_std_20)
  xenon_enable_guest_fp_semantics(xenon_cpu_generated_compile PRIVATE)
  add_dependencies(xenon_cpu_generated_compile xenon_cpu_codegen_all)

  add_executable(xenon_cpu_codegen_smoke cpu/codegen/codegen_smoke.cpp)
  target_link_libraries(xenon_cpu_codegen_smoke PRIVATE xenon_cpu)
  set(XENON_GENERATED_SMOKE "${CMAKE_CURRENT_BINARY_DIR}/generated_native_smoke.cpp")
  add_custom_command(
    OUTPUT "${XENON_GENERATED_SMOKE}"
    COMMAND xenon_cpu_codegen_smoke "${XENON_GENERATED_SMOKE}"
    DEPENDS xenon_cpu_codegen_smoke
    VERBATIM)
  add_executable(xenon_cpu_native_smoke
    cpu/x86_64/native_smoke.cpp
    "${XENON_GENERATED_SMOKE}")
  target_link_libraries(xenon_cpu_native_smoke PRIVATE xenon_cpu)
  xenon_enable_test_asserts(xenon_cpu_native_smoke)
  add_test(NAME xenon_cpu_native_smoke COMMAND xenon_cpu_native_smoke)

  add_executable(xenon_cpu_exhaustive_native
    cpu/x86_64/exhaustive_native.cpp
    $<TARGET_OBJECTS:xenon_cpu_generated_compile>)
  target_link_libraries(xenon_cpu_exhaustive_native PRIVATE xenon_cpu)
  add_dependencies(xenon_cpu_exhaustive_native xenon_cpu_generated_compile)
  xenon_enable_test_asserts(xenon_cpu_exhaustive_native)
  add_test(NAME xenon_cpu_exhaustive_native COMMAND xenon_cpu_exhaustive_native)

  function(xenon_add_generated_cpu_test name generator source)
    add_executable(xenon_cpu_codegen_${name} "cpu/codegen/${generator}")
    target_link_libraries(xenon_cpu_codegen_${name} PRIVATE xenon_cpu)
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated_${name}.cpp")
    add_custom_command(
      OUTPUT "${generated}"
      COMMAND xenon_cpu_codegen_${name} "${generated}"
      DEPENDS xenon_cpu_codegen_${name}
      VERBATIM)
    add_executable(xenon_cpu_${name} "cpu/x86_64/${source}" "${generated}")
    target_link_libraries(xenon_cpu_${name} PRIVATE xenon_cpu)
    xenon_enable_test_asserts(xenon_cpu_${name})
    add_test(NAME xenon_cpu_${name} COMMAND xenon_cpu_${name})
  endfunction()

  add_executable(xenon_cpu_codegen_setjmp_longjmp
    cpu/codegen/codegen_setjmp_longjmp.cpp)
  target_link_libraries(xenon_cpu_codegen_setjmp_longjmp PRIVATE xenon_cpu)
  set(XENON_GENERATED_SETJMP_LONGJMP
    "${CMAKE_CURRENT_BINARY_DIR}/generated_setjmp_longjmp.cpp")
  add_custom_command(
    OUTPUT "${XENON_GENERATED_SETJMP_LONGJMP}"
    COMMAND xenon_cpu_codegen_setjmp_longjmp "${XENON_GENERATED_SETJMP_LONGJMP}"
    DEPENDS xenon_cpu_codegen_setjmp_longjmp
    VERBATIM)
  add_executable(xenon_cpu_setjmp_longjmp
    cpu/x86_64/setjmp_longjmp.cpp
    "${XENON_GENERATED_SETJMP_LONGJMP}")
  target_link_libraries(xenon_cpu_setjmp_longjmp PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_cpu_setjmp_longjmp)
  add_test(NAME xenon_cpu_setjmp_longjmp COMMAND xenon_cpu_setjmp_longjmp)

  add_executable(xenon_cpu_runtime_helpers_register_range
    cpu/x86_64/runtime_helpers_register_range.cpp)
  target_link_libraries(xenon_cpu_runtime_helpers_register_range PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_cpu_runtime_helpers_register_range)
  add_test(NAME xenon_cpu_runtime_helpers_register_range COMMAND xenon_cpu_runtime_helpers_register_range)

  # Gen 8 semantic verification: an independent PowerPC reference model is
  # differentially checked against real generated AOT code using deterministic
  # randomized CPU/memory states.  The framework test also proves exact-seed
  # replay, bounded failure minimization, and repro-artifact emission.
  add_executable(xenon_cpu_semantic_verifier_tests
    cpu/verification/semantic_verifier_tests.cpp)
  target_link_libraries(xenon_cpu_semantic_verifier_tests PRIVATE xenon_cpu)
  xenon_enable_test_asserts(xenon_cpu_semantic_verifier_tests)
  add_test(NAME xenon_cpu_semantic_verifier_tests COMMAND xenon_cpu_semantic_verifier_tests)

  add_executable(xenon_cpu_codegen_semantic_verification
    cpu/codegen/codegen_semantic_verification.cpp)
  target_include_directories(xenon_cpu_codegen_semantic_verification PRIVATE
    "${XENON_SOURCE_DIR}/tests/cpu/verification")
  target_link_libraries(xenon_cpu_codegen_semantic_verification PRIVATE xenon_cpu)
  set(XENON_GENERATED_SEMANTIC_VERIFICATION
    "${CMAKE_CURRENT_BINARY_DIR}/generated_semantic_verification.cpp")
  add_custom_command(
    OUTPUT "${XENON_GENERATED_SEMANTIC_VERIFICATION}"
    COMMAND xenon_cpu_codegen_semantic_verification "${XENON_GENERATED_SEMANTIC_VERIFICATION}"
    DEPENDS xenon_cpu_codegen_semantic_verification
    VERBATIM)
  add_executable(xenon_cpu_semantic_verification
    cpu/x86_64/semantic_verification.cpp
    "${XENON_GENERATED_SEMANTIC_VERIFICATION}")
  target_include_directories(xenon_cpu_semantic_verification PRIVATE
    "${XENON_SOURCE_DIR}/tests/cpu/verification")
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
  xenon_add_generated_cpu_test(local_indirect codegen_local_indirect.cpp local_indirect.cpp)
  xenon_add_generated_cpu_test(direct_call codegen_direct_call.cpp direct_call.cpp)

  # XAM tests
  add_executable(xenon_xam_user_manager_tests xam/user_manager_tests.cpp)
  target_link_libraries(xenon_xam_user_manager_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_xam_user_manager_tests)
  add_test(NAME xenon_xam_user_manager_tests COMMAND xenon_xam_user_manager_tests)

  add_executable(xenon_xam_export_ordinal_tests xam/xam_export_ordinal_tests.cpp)
  target_link_libraries(xenon_xam_export_ordinal_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_xam_export_ordinal_tests)
  add_test(NAME xenon_xam_export_ordinal_tests COMMAND xenon_xam_export_ordinal_tests)

  add_executable(xenon_guest_enumerator_tests xam/guest_enumerator_tests.cpp)
  target_link_libraries(xenon_guest_enumerator_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_guest_enumerator_tests)
  add_test(NAME xenon_guest_enumerator_tests COMMAND xenon_guest_enumerator_tests)

  if(XENON_ENABLE_INPUT)
    add_executable(xenon_input_tests input/input_tests.cpp)
    target_link_libraries(xenon_input_tests PRIVATE xenon_input)
    xenon_enable_test_asserts(xenon_input_tests)
    add_test(NAME xenon_input_tests COMMAND xenon_input_tests)

    add_executable(xenon_frontend_router_tests input/frontend_router_tests.cpp)
    target_link_libraries(xenon_frontend_router_tests PRIVATE xenon_input)
    xenon_enable_test_asserts(xenon_frontend_router_tests)
    add_test(NAME xenon_frontend_router_tests COMMAND xenon_frontend_router_tests)

    foreach(test_name IN ITEMS sdl_driver keyboard_mouse xam_facade profile advanced module_api)
      set(target "xenon_input_${test_name}_tests")
      add_executable(${target} "input/${test_name}_tests.cpp")
      target_link_libraries(${target} PRIVATE xenon_input)
      xenon_enable_test_asserts(${target})
      add_test(NAME ${target} COMMAND ${target})
    endforeach()
    if(TARGET xenon_input_guest)
      add_executable(xenon_input_xam_guest_tests input/xam_guest_tests.cpp)
      target_link_libraries(xenon_input_xam_guest_tests PRIVATE xenon_input_guest)
      xenon_enable_test_asserts(xenon_input_xam_guest_tests)
      add_test(NAME xenon_input_xam_guest_tests COMMAND xenon_input_xam_guest_tests)
    endif()
  endif()

  if(XENON_ENABLE_MEMORY)
    add_executable(xenon_memory_tests memory/memory_tests.cpp)
    target_link_libraries(xenon_memory_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_memory_tests)
    add_test(NAME xenon_memory_tests COMMAND xenon_memory_tests)

    add_executable(xenon_memory_hardening_tests
      memory/memory_hardening_tests.cpp)
    target_link_libraries(xenon_memory_hardening_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_memory_hardening_tests)
    add_test(NAME xenon_memory_hardening_tests
      COMMAND xenon_memory_hardening_tests)

    add_executable(xenon_memory_ordering_tests
      memory/memory_ordering_tests.cpp)
    target_link_libraries(xenon_memory_ordering_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_memory_ordering_tests)
    add_test(NAME xenon_memory_ordering_tests COMMAND xenon_memory_ordering_tests)

    add_executable(xenon_memory_type_tests
      memory/memory_type_tests.cpp)
    target_link_libraries(xenon_memory_type_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_memory_type_tests)
    add_test(NAME xenon_memory_type_tests COMMAND xenon_memory_type_tests)

    add_executable(xenon_mmio_tests
      memory/mmio_tests.cpp)
    target_link_libraries(xenon_mmio_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_mmio_tests)
    add_test(NAME xenon_mmio_tests COMMAND xenon_mmio_tests)

    add_executable(xenon_memory_fault_tests
      memory/fault_tests.cpp)
    target_link_libraries(xenon_memory_fault_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_memory_fault_tests)
    add_test(NAME xenon_memory_fault_tests COMMAND xenon_memory_fault_tests)

    add_executable(xenon_dynamic_fallback_tests
      cpu/dynamic_fallback_tests.cpp)
    target_link_libraries(xenon_dynamic_fallback_tests PRIVATE xenon_cpu xenon_memory)
    xenon_enable_test_asserts(xenon_dynamic_fallback_tests)
    add_test(NAME xenon_dynamic_fallback_tests COMMAND xenon_dynamic_fallback_tests)

    add_executable(xenon_dynamic_fallback_semantic_tests
      cpu/verification/dynamic_fallback_semantic_tests.cpp)
    target_include_directories(xenon_dynamic_fallback_semantic_tests PRIVATE
      "${XENON_SOURCE_DIR}/tests/cpu/verification")
    target_link_libraries(xenon_dynamic_fallback_semantic_tests PRIVATE xenon_cpu xenon_memory)
    xenon_enable_test_asserts(xenon_dynamic_fallback_semantic_tests)
    add_test(NAME xenon_dynamic_fallback_semantic_tests COMMAND xenon_dynamic_fallback_semantic_tests)

    add_executable(xenon_executable_code_cache_tests
      memory/executable_code_cache_tests.cpp)
    target_link_libraries(xenon_executable_code_cache_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_executable_code_cache_tests)
    add_test(NAME xenon_executable_code_cache_tests
      COMMAND xenon_executable_code_cache_tests)

    add_executable(xenon_host_vm_tests memory/host_vm_tests.cpp)
    target_link_libraries(xenon_host_vm_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_host_vm_tests)
    add_test(NAME xenon_host_vm_tests COMMAND xenon_host_vm_tests)

    add_executable(xenon_gpu_coherency_tests
      memory/gpu_coherency_tests.cpp)
    target_link_libraries(xenon_gpu_coherency_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_gpu_coherency_tests)
    add_test(NAME xenon_gpu_coherency_tests COMMAND xenon_gpu_coherency_tests)

    add_executable(xenon_memory_codegen_integration memory/codegen_memory_integration.cpp)
    target_link_libraries(xenon_memory_codegen_integration PRIVATE xenon_cpu)
    set(XENON_GENERATED_MEMORY_INTEGRATION
      "${CMAKE_CURRENT_BINARY_DIR}/generated_memory_integration.cpp")
    add_custom_command(
      OUTPUT "${XENON_GENERATED_MEMORY_INTEGRATION}"
      COMMAND xenon_memory_codegen_integration "${XENON_GENERATED_MEMORY_INTEGRATION}"
      DEPENDS xenon_memory_codegen_integration
      VERBATIM)
    add_executable(xenon_memory_cpu_integration
      memory/memory_cpu_integration.cpp
      "${XENON_GENERATED_MEMORY_INTEGRATION}")
    target_link_libraries(xenon_memory_cpu_integration PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_memory_cpu_integration)
    add_test(NAME xenon_memory_cpu_integration COMMAND xenon_memory_cpu_integration)
  endif()

  if(XENON_ENABLE_GRAPHICS)
    add_executable(xenon_backend_capability_tests
      graphics/backends/backend_capabilities.cpp)
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
      graphics/backends/backend_ownership_integration.cpp)
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
      graphics/backends/cross_backend_canonical.cpp)
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

    add_executable(xenon_shader_decoder_tests graphics/xenos/shader_decoder.cpp)
    set_target_properties(xenon_shader_decoder_tests PROPERTIES
      OUTPUT_NAME xenon_shader_decoder_gpu09_tests)
    target_link_libraries(xenon_shader_decoder_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_shader_decoder_tests)
    add_test(NAME xenon_shader_decoder_tests COMMAND xenon_shader_decoder_tests)

    add_executable(xenon_depth_format_tests graphics/xenos/depth_format.cpp)
    target_link_libraries(xenon_depth_format_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_depth_format_tests)
    add_test(NAME xenon_depth_format_tests COMMAND xenon_depth_format_tests)

    add_executable(xenon_shader_lowering_tests
      graphics/xenos/shader_lowering.cpp)
    target_link_libraries(xenon_shader_lowering_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_shader_lowering_tests)
    add_test(NAME xenon_shader_lowering_tests COMMAND xenon_shader_lowering_tests)

    add_executable(xenon_resource_ir_tests graphics/xenos/resource_ir.cpp)
    target_link_libraries(xenon_resource_ir_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_resource_ir_tests)
    add_test(NAME xenon_resource_ir_tests COMMAND xenon_resource_ir_tests)

    add_executable(xenon_resource_barrier_tests
      graphics/xenos/resource_barrier.cpp)
    target_link_libraries(xenon_resource_barrier_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_resource_barrier_tests)
    add_test(NAME xenon_resource_barrier_tests COMMAND xenon_resource_barrier_tests)

    add_executable(xenon_texture_tests graphics/xenos/texture.cpp)
    target_link_libraries(xenon_texture_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_texture_tests)
    add_test(NAME xenon_texture_tests COMMAND xenon_texture_tests)

    add_executable(xenon_presentation_tests graphics/xenos/presentation.cpp)
    target_link_libraries(xenon_presentation_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_presentation_tests)
    add_test(NAME xenon_presentation_tests COMMAND xenon_presentation_tests)

    add_executable(xenon_edram_surface_tests
      graphics/xenos/edram_surface.cpp)
    target_link_libraries(xenon_edram_surface_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_edram_surface_tests)
    add_test(NAME xenon_edram_surface_tests COMMAND xenon_edram_surface_tests)

    add_executable(xenon_edram_ownership_tests
      graphics/xenos/edram_ownership.cpp)
    target_link_libraries(xenon_edram_ownership_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_edram_ownership_tests)
    add_test(NAME xenon_edram_ownership_tests COMMAND xenon_edram_ownership_tests)

    add_executable(xenon_primitive_processor_tests
      graphics/xenos/primitive_processor.cpp)
    target_link_libraries(xenon_primitive_processor_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_primitive_processor_tests)
    add_test(NAME xenon_primitive_processor_tests COMMAND xenon_primitive_processor_tests)

    if(TARGET xenon_graphics_dxc)
      add_executable(xenon_shader_translation_tests
        graphics/xenos/shader_translation.cpp)
      set_target_properties(xenon_shader_translation_tests PROPERTIES
        OUTPUT_NAME xenon_shader_translation_gpu07_tests)
      target_link_libraries(xenon_shader_translation_tests PRIVATE xenon_graphics_dxc)
      xenon_enable_test_asserts(xenon_shader_translation_tests)
      xenon_stage_runtime_dependencies(xenon_shader_translation_tests)
      add_test(NAME xenon_shader_translation_tests COMMAND xenon_shader_translation_tests)
    endif()

    add_executable(xenon_gpu_frontend_tests graphics/xenos/gpu_frontend.cpp)
    set_target_properties(xenon_gpu_frontend_tests PROPERTIES
      OUTPUT_NAME xenon_gpu_frontend_gpu08_tests)
    target_link_libraries(xenon_gpu_frontend_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_gpu_frontend_tests)
    add_test(NAME xenon_gpu_frontend_tests COMMAND xenon_gpu_frontend_tests)
  endif()

  # Keep standalone regressions attached to a build target and CTest. The
  # source ownership audit checks this contract for every test main().
  xenon_add_standalone_test(xenon_export_trace_tests
    core/export_trace_tests.cpp xenon_core)
  xenon_add_standalone_test(xenon_guest_memory_watch_tests
    core/guest_memory_watch_tests.cpp xenon_core)

  xenon_add_standalone_test(xenon_cpu_v2_memory_integration_tests
    cpu/codegen/cpu_v2_memory_integration.cpp xenon_cpu)
  foreach(test_name IN ITEMS control_flow effective_address flags optimizer)
    xenon_add_standalone_test(xenon_cpu_v2_${test_name}_tests
      cpu/optimizer/cpu_v2_${test_name}.cpp xenon_cpu)
  endforeach()
  xenon_add_standalone_test(xenon_fallback_lifter_parity_tests
    cpu/fallback_lifter_parity_tests.cpp xenon_recomp)
  xenon_add_standalone_test(xenon_cpu_coverage_tests
    recomp/cpu_coverage_tests.cpp xenon_recomp)

  if(TARGET xenon_memory)
    xenon_add_standalone_test(xenon_dynamic_fallback_rotate_tests
      cpu/dynamic_fallback_rotate_tests.cpp xenon_cpu xenon_memory)
    xenon_add_standalone_test(xenon_dynamic_fallback_parity_semantics_tests
      cpu/dynamic_fallback_parity_semantics_tests.cpp xenon_cpu xenon_memory)
  endif()
  if(WIN32 AND TARGET xenon_memory)
    xenon_add_standalone_test(xenon_xam_net_socket_tests
      xam/xam_net_socket_tests.cpp xenon_core)
  endif()
  if(TARGET xenon_kernel)
    xenon_add_standalone_test(xenon_kernel_infinite_wait_tests
      kernel/infinite_wait_tests.cpp xenon_kernel)
  endif()
  if(TARGET xenon_graphics)
    xenon_add_standalone_test(xenon_xenos_register_aperture_tests
      graphics/xenos/register_aperture_tests.cpp xenon_graphics xenon_memory)
  endif()

  xenon_add_standalone_test(xenon_xam_content_services_tests
    xam/content_services_tests.cpp xenon_core)
  foreach(test_name IN ITEMS module_lookup ob_exports pool_debug
      rtl_string_family string_format threading_exports virtual_memory_export)
    xenon_add_standalone_test(xenon_xbox_${test_name}_tests
      xbox/${test_name}_tests.cpp xenon_core)
  endforeach()
endif()

if(XENON_BUILD_TESTS AND TARGET xenon_runtime_host)
  # LaunchConfig parsing/validation, isolated from process-spawning and Qt
  # (see launcher/tests/runtime/runtime_bridge_tests.cpp for the IPC-lifecycle
  # side of the same contract).
  add_executable(xenon_runtime_host_tests
    runtime_host/launch_config_tests.cpp
    ${XENON_SOURCE_DIR}/runtime_host/src/launch_config.cpp
    ${XENON_SOURCE_DIR}/runtime_host/src/launch_config.hpp
  )
  target_include_directories(xenon_runtime_host_tests PRIVATE ${XENON_SOURCE_DIR}/runtime_host/src)
  target_link_libraries(xenon_runtime_host_tests PRIVATE xenon_core)
  target_compile_features(xenon_runtime_host_tests PRIVATE cxx_std_20)
  xenon_enable_test_asserts(xenon_runtime_host_tests)
  add_test(NAME xenon_runtime_host_tests COMMAND xenon_runtime_host_tests)

  add_executable(xenon_runtime_content_source_tests
    runtime_host/content_source_tests.cpp
    ${XENON_SOURCE_DIR}/runtime_host/src/content_source.cpp
    ${XENON_SOURCE_DIR}/runtime_host/src/content_source.hpp
  )
  target_include_directories(xenon_runtime_content_source_tests PRIVATE ${XENON_SOURCE_DIR}/runtime_host/src)
  target_link_libraries(xenon_runtime_content_source_tests PRIVATE xenon_core)
  target_compile_features(xenon_runtime_content_source_tests PRIVATE cxx_std_20)
  xenon_enable_test_asserts(xenon_runtime_content_source_tests)
  add_test(NAME xenon_runtime_content_source_tests COMMAND xenon_runtime_content_source_tests)

  # Part 5.7 (Gracemeria readiness pass): structured launch-failure
  # categories in status.json - see tests/runtime_host/status_writer_tests.cpp.
  add_executable(xenon_status_writer_tests
    runtime_host/status_writer_tests.cpp
    ${XENON_SOURCE_DIR}/runtime_host/src/status_writer.cpp
    ${XENON_SOURCE_DIR}/runtime_host/src/status_writer.hpp
    ${XENON_SOURCE_DIR}/runtime_host/src/launch_config.cpp
    ${XENON_SOURCE_DIR}/runtime_host/src/launch_config.hpp
  )
  target_include_directories(xenon_status_writer_tests PRIVATE ${XENON_SOURCE_DIR}/runtime_host/src)
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
    runtime_host/presentation_host_tests.cpp
    ${XENON_SOURCE_DIR}/runtime_host/src/presentation_host.cpp
    ${XENON_SOURCE_DIR}/runtime_host/src/presentation_host.hpp
  )
  target_include_directories(xenon_presentation_host_tests PRIVATE ${XENON_SOURCE_DIR}/runtime_host/src)
  target_link_libraries(xenon_presentation_host_tests PRIVATE xenon_core ${XENON_PRESENTATION_HOST_TESTS_SDL2_TARGET})
  target_compile_features(xenon_presentation_host_tests PRIVATE cxx_std_20)
  xenon_enable_test_asserts(xenon_presentation_host_tests)
  add_test(NAME xenon_presentation_host_tests COMMAND xenon_presentation_host_tests)
endif()
