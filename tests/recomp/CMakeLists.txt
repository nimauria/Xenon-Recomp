# Recomp Analysis V2 synthetic benchmark (Part 20): jobs=1 vs jobs=auto wall
# time on a large synthetic independent-function workload, no copyrighted
# content. Not registered as a ctest (it prints timing, not pass/fail) -
# run manually, e.g. `xenon_recomp_analysis_benchmark 5000 24`.
if(XENON_BUILD_BENCHMARKS)
  add_executable(xenon_recomp_analysis_benchmark
    recomp_analysis_benchmark.cpp)
  target_link_libraries(xenon_recomp_analysis_benchmark PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_recomp_analysis_benchmark)
endif()

if(XENON_BUILD_TESTS)

  add_executable(xenon_recomp_driver_tests recomp_driver_tests.cpp)
  target_link_libraries(xenon_recomp_driver_tests PRIVATE xenon_recomp)
  target_compile_definitions(xenon_recomp_driver_tests PRIVATE
    XENON_SOURCE_ROOT="${CMAKE_SOURCE_DIR}")
  xenon_enable_test_asserts(xenon_recomp_driver_tests)
  add_test(NAME xenon_recomp_driver_tests COMMAND xenon_recomp_driver_tests)

  # Generic worker pool (Part 3 of the Recomp Analysis V2 pass): correctness
  # of parallel_for() itself (every index run exactly once, result ordering
  # independent of completion order, exception propagation, no races under
  # repeated stress runs), independent of the Recomp Driver that consumes it.
  add_executable(xenon_worker_pool_tests worker_pool_tests.cpp)
  target_link_libraries(xenon_worker_pool_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_worker_pool_tests)
  add_test(NAME xenon_worker_pool_tests COMMAND xenon_worker_pool_tests)

  # Parallel analysis determinism (Part 4/21): jobs=1 vs jobs=N produce
  # byte-identical deterministic AnalysisReport content on a synthetic
  # multi-function fixture, and discovery waves converge correctly.
  add_executable(xenon_recomp_parallel_analysis_tests parallel_analysis_tests.cpp)
  target_link_libraries(xenon_recomp_parallel_analysis_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_recomp_parallel_analysis_tests)
  add_test(NAME xenon_recomp_parallel_analysis_tests COMMAND xenon_recomp_parallel_analysis_tests)

  # Recomp Analysis V3 (discovery-quality pass): provenance/evidence
  # combination, confidence, generic TLS-callback seeding, bounded CTR
  # dataflow resolution, invalid-vs-unsupported PPC classification, the
  # conservative prologue heuristic, validated tail calls, and
  # switch/jump-table bounds-check gating.
  add_executable(xenon_discovery_quality_tests discovery_quality_tests.cpp)
  target_link_libraries(xenon_discovery_quality_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_discovery_quality_tests)
  add_test(NAME xenon_discovery_quality_tests COMMAND xenon_discovery_quality_tests)

  # Gen 6 PPC value/indirect-flow analysis: bounded abstract-value lattice,
  # cheap block-local propagation, read-only pointer/vtable folding, and the
  # selective bounded backward-slice resolver.
  add_executable(xenon_ppc_value_analysis_tests ppc_value_analysis_tests.cpp)
  target_link_libraries(xenon_ppc_value_analysis_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_ppc_value_analysis_tests)
  add_test(NAME xenon_ppc_value_analysis_tests COMMAND xenon_ppc_value_analysis_tests)

  # Gen 9 universal knowledge base: versioned normalized fingerprints,
  # cross-revision matching, confidence gates, JSONL persistence and replay.
  find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
  add_test(NAME xenon_native_cache_tests COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/native_cache_tests.py")
  set_tests_properties(xenon_native_cache_tests PROPERTIES ENVIRONMENT "XENON_TEST_CXX=${CMAKE_CXX_COMPILER}")
  add_executable(xenon_compilation_graph_tests compilation_graph_tests.cpp)
  target_link_libraries(xenon_compilation_graph_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_compilation_graph_tests)
  add_test(NAME xenon_compilation_graph_tests COMMAND xenon_compilation_graph_tests)

  # Gen 11 autonomous game intake: multi-executable discovery over a
  # directory, a loose .xex file and a synthetic disc image, plus the Game
  # Compilation Graph's JSON shape.
  add_executable(xenon_game_intake_tests game_intake_tests.cpp)
  target_link_libraries(xenon_game_intake_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_game_intake_tests)
  add_test(NAME xenon_game_intake_tests COMMAND xenon_game_intake_tests)

  add_executable(xenon_knowledge_base_tests knowledge_base_tests.cpp)
  target_link_libraries(xenon_knowledge_base_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_knowledge_base_tests)
  add_test(NAME xenon_knowledge_base_tests COMMAND xenon_knowledge_base_tests)

  # Prepared native-module artifact cache (automatic game preparation
  # pipeline): digest/invalidation semantics and real stage->validate->
  # atomic-promote/rollback behaviour, including a real compiled-and-loaded
  # shared library check (not a stubbed file-existence check).
  add_executable(xenon_artifact_cache_tests artifact_cache_tests.cpp)
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
  add_executable(xenon_prepare_worker_tests xenon_prepare_worker_tests.cpp)
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
  add_executable(xenon_analysis_schema_tests analysis_schema_tests.cpp)
  target_link_libraries(xenon_analysis_schema_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_analysis_schema_tests)
  add_test(NAME xenon_analysis_schema_tests COMMAND xenon_analysis_schema_tests)

  # Analysis Hint Schema V2 consumption: proves the Recomp Driver actually
  # uses chunks/switch tables/known indirect calls/native replacements/
  # regions during real analysis, not merely parses/stores them (Part 1.11).
  add_executable(xenon_heap_native_replacement_tests
    heap_native_replacement_tests.cpp)
  target_link_libraries(xenon_heap_native_replacement_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_heap_native_replacement_tests)
  add_test(NAME xenon_heap_native_replacement_tests
    COMMAND xenon_heap_native_replacement_tests)

  add_executable(xenon_analysis_v2_consumption_tests analysis_v2_consumption_tests.cpp)
  target_link_libraries(xenon_analysis_v2_consumption_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_analysis_v2_consumption_tests)
  add_test(NAME xenon_analysis_v2_consumption_tests COMMAND xenon_analysis_v2_consumption_tests)

  # Generated-code deduplication / shard ownership fix: proves the actual
  # production bug (byte-identical functions colliding in the codegen
  # content-addressed cache, producing MSVC C2084 duplicate-body errors on a
  # real title) stays fixed, plus the FunctionChunk/independent-FunctionHint
  # mutual-exclusion fix, parallel codegen determinism, and the
  # fail-fast symbol/address uniqueness validation added alongside it.
  add_executable(xenon_codegen_ownership_tests codegen_ownership_tests.cpp)
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
  add_executable(xenon_registry_numeric_format_tests registry_numeric_format_tests.cpp)
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
  add_executable(xenon_import_thunk_analysis_tests import_thunk_analysis_tests.cpp)
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
  add_executable(xenon_tail_call_gap_discovery_tests tail_call_gap_discovery_tests.cpp)
  target_link_libraries(xenon_tail_call_gap_discovery_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_tail_call_gap_discovery_tests)
  add_test(NAME xenon_tail_call_gap_discovery_tests COMMAND xenon_tail_call_gap_discovery_tests)

  # ModuleHintProvider production integration (Part 2/7): a synthetic
  # two-revision module package, correct/incorrect revision selection, and
  # proof the provider is actually consumed by load_and_analyze().
  add_executable(xenon_module_hint_provider_tests module_hint_provider_tests.cpp)
  target_link_libraries(xenon_module_hint_provider_tests PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_module_hint_provider_tests)
  add_test(NAME xenon_module_hint_provider_tests COMMAND xenon_module_hint_provider_tests)
  xenon_add_standalone_test(xenon_cpu_coverage_tests
    cpu_coverage_tests.cpp xenon_recomp)
endif()
