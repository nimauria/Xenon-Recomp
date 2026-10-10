if(XENON_BUILD_TESTS)

  add_executable(xenon_cpu_tests ppc/cpu_tests.cpp)
  target_link_libraries(xenon_cpu_tests PRIVATE xenon_cpu)
  target_include_directories(xenon_cpu_tests PRIVATE "${XENON_SOURCE_DIR}/src/cpu/ppc/decoder")
  xenon_enable_test_asserts(xenon_cpu_tests)
  add_test(NAME xenon_cpu_tests COMMAND xenon_cpu_tests)

  add_executable(xenon_cpu_v2_cfg ir/cpu_v2_cfg.cpp)
  target_link_libraries(xenon_cpu_v2_cfg PRIVATE xenon_cpu)
  xenon_enable_test_asserts(xenon_cpu_v2_cfg)
  add_test(NAME xenon_cpu_v2_cfg COMMAND xenon_cpu_v2_cfg)

  add_executable(xenon_cpu_v2_ir ir/cpu_v2_ir.cpp)
  target_link_libraries(xenon_cpu_v2_ir PRIVATE xenon_cpu)
  xenon_enable_test_asserts(xenon_cpu_v2_ir)
  add_test(NAME xenon_cpu_v2_ir COMMAND xenon_cpu_v2_ir)

  add_executable(xenon_cpu_v2_state_promotion optimizer/cpu_v2_state_promotion.cpp)
  target_link_libraries(xenon_cpu_v2_state_promotion PRIVATE xenon_cpu)
  xenon_enable_test_asserts(xenon_cpu_v2_state_promotion)
  add_test(NAME xenon_cpu_v2_state_promotion COMMAND xenon_cpu_v2_state_promotion)

  # Generate and compile one function for every canonical instruction.
  add_executable(xenon_cpu_codegen_all codegen/codegen_all.cpp)
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

  add_executable(xenon_cpu_codegen_smoke codegen/codegen_smoke.cpp)
  target_link_libraries(xenon_cpu_codegen_smoke PRIVATE xenon_cpu)
  set(XENON_GENERATED_SMOKE "${CMAKE_CURRENT_BINARY_DIR}/generated_native_smoke.cpp")
  add_custom_command(
    OUTPUT "${XENON_GENERATED_SMOKE}"
    COMMAND xenon_cpu_codegen_smoke "${XENON_GENERATED_SMOKE}"
    DEPENDS xenon_cpu_codegen_smoke
    VERBATIM)
  add_executable(xenon_cpu_native_smoke
    x86_64/native_smoke.cpp
    "${XENON_GENERATED_SMOKE}")
  target_link_libraries(xenon_cpu_native_smoke PRIVATE xenon_cpu)
  xenon_enable_test_asserts(xenon_cpu_native_smoke)
  add_test(NAME xenon_cpu_native_smoke COMMAND xenon_cpu_native_smoke)

  add_executable(xenon_cpu_exhaustive_native
    x86_64/exhaustive_native.cpp
    $<TARGET_OBJECTS:xenon_cpu_generated_compile>)
  target_link_libraries(xenon_cpu_exhaustive_native PRIVATE xenon_cpu)
  add_dependencies(xenon_cpu_exhaustive_native xenon_cpu_generated_compile)
  xenon_enable_test_asserts(xenon_cpu_exhaustive_native)
  add_test(NAME xenon_cpu_exhaustive_native COMMAND xenon_cpu_exhaustive_native)

  function(xenon_add_generated_cpu_test name generator source)
    add_executable(xenon_cpu_codegen_${name} "codegen/${generator}")
    target_link_libraries(xenon_cpu_codegen_${name} PRIVATE xenon_cpu)
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated_${name}.cpp")
    add_custom_command(
      OUTPUT "${generated}"
      COMMAND xenon_cpu_codegen_${name} "${generated}"
      DEPENDS xenon_cpu_codegen_${name}
      VERBATIM)
    add_executable(xenon_cpu_${name} "x86_64/${source}" "${generated}")
    target_link_libraries(xenon_cpu_${name} PRIVATE xenon_cpu)
    xenon_enable_test_asserts(xenon_cpu_${name})
    add_test(NAME xenon_cpu_${name} COMMAND xenon_cpu_${name})
  endfunction()

  add_executable(xenon_cpu_codegen_setjmp_longjmp
    codegen/codegen_setjmp_longjmp.cpp)
  target_link_libraries(xenon_cpu_codegen_setjmp_longjmp PRIVATE xenon_cpu)
  set(XENON_GENERATED_SETJMP_LONGJMP
    "${CMAKE_CURRENT_BINARY_DIR}/generated_setjmp_longjmp.cpp")
  add_custom_command(
    OUTPUT "${XENON_GENERATED_SETJMP_LONGJMP}"
    COMMAND xenon_cpu_codegen_setjmp_longjmp "${XENON_GENERATED_SETJMP_LONGJMP}"
    DEPENDS xenon_cpu_codegen_setjmp_longjmp
    VERBATIM)
  add_executable(xenon_cpu_setjmp_longjmp
    x86_64/setjmp_longjmp.cpp
    "${XENON_GENERATED_SETJMP_LONGJMP}")
  target_link_libraries(xenon_cpu_setjmp_longjmp PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_cpu_setjmp_longjmp)
  add_test(NAME xenon_cpu_setjmp_longjmp COMMAND xenon_cpu_setjmp_longjmp)

  add_executable(xenon_cpu_runtime_helpers_register_range
    x86_64/runtime_helpers_register_range.cpp)
  target_link_libraries(xenon_cpu_runtime_helpers_register_range PRIVATE xenon_recomp)
  xenon_enable_test_asserts(xenon_cpu_runtime_helpers_register_range)
  add_test(NAME xenon_cpu_runtime_helpers_register_range COMMAND xenon_cpu_runtime_helpers_register_range)

  # Gen 8 semantic verification: an independent PowerPC reference model is
  # differentially checked against real generated AOT code using deterministic
  # randomized CPU/memory states.  The framework test also proves exact-seed
  # replay, bounded failure minimization, and repro-artifact emission.
  add_executable(xenon_cpu_semantic_verifier_tests
    verification/semantic_verifier_tests.cpp)
  target_link_libraries(xenon_cpu_semantic_verifier_tests PRIVATE xenon_cpu)
  xenon_enable_test_asserts(xenon_cpu_semantic_verifier_tests)
  add_test(NAME xenon_cpu_semantic_verifier_tests COMMAND xenon_cpu_semantic_verifier_tests)

  add_executable(xenon_cpu_codegen_semantic_verification
    codegen/codegen_semantic_verification.cpp)
  target_include_directories(xenon_cpu_codegen_semantic_verification PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/verification")
  target_link_libraries(xenon_cpu_codegen_semantic_verification PRIVATE xenon_cpu)
  set(XENON_GENERATED_SEMANTIC_VERIFICATION
    "${CMAKE_CURRENT_BINARY_DIR}/generated_semantic_verification.cpp")
  add_custom_command(
    OUTPUT "${XENON_GENERATED_SEMANTIC_VERIFICATION}"
    COMMAND xenon_cpu_codegen_semantic_verification "${XENON_GENERATED_SEMANTIC_VERIFICATION}"
    DEPENDS xenon_cpu_codegen_semantic_verification
    VERBATIM)
  add_executable(xenon_cpu_semantic_verification
    x86_64/semantic_verification.cpp
    "${XENON_GENERATED_SEMANTIC_VERIFICATION}")
  target_include_directories(xenon_cpu_semantic_verification PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/verification")
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
  if(XENON_ENABLE_MEMORY)

    add_executable(xenon_dynamic_fallback_tests
      dynamic_fallback_tests.cpp)
    target_link_libraries(xenon_dynamic_fallback_tests PRIVATE xenon_cpu xenon_memory)
    xenon_enable_test_asserts(xenon_dynamic_fallback_tests)
    add_test(NAME xenon_dynamic_fallback_tests COMMAND xenon_dynamic_fallback_tests)

    add_executable(xenon_dynamic_fallback_semantic_tests
      verification/dynamic_fallback_semantic_tests.cpp)
    target_include_directories(xenon_dynamic_fallback_semantic_tests PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/verification")
    target_link_libraries(xenon_dynamic_fallback_semantic_tests PRIVATE xenon_cpu xenon_memory)
    xenon_enable_test_asserts(xenon_dynamic_fallback_semantic_tests)
    add_test(NAME xenon_dynamic_fallback_semantic_tests COMMAND xenon_dynamic_fallback_semantic_tests)
  endif()

  xenon_add_standalone_test(xenon_cpu_v2_memory_integration_tests
    codegen/cpu_v2_memory_integration.cpp xenon_cpu)
  foreach(test_name IN ITEMS control_flow effective_address flags optimizer)
    xenon_add_standalone_test(xenon_cpu_v2_${test_name}_tests
      optimizer/cpu_v2_${test_name}.cpp xenon_cpu)
  endforeach()
  xenon_add_standalone_test(xenon_fallback_lifter_parity_tests
    fallback_lifter_parity_tests.cpp xenon_recomp)

  if(TARGET xenon_memory)
    xenon_add_standalone_test(xenon_dynamic_fallback_rotate_tests
      dynamic_fallback_rotate_tests.cpp xenon_cpu xenon_memory)
    xenon_add_standalone_test(xenon_dynamic_fallback_parity_semantics_tests
      dynamic_fallback_parity_semantics_tests.cpp xenon_cpu xenon_memory)
  endif()
endif()
