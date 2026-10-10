if(XENON_BUILD_BENCHMARKS AND XENON_ENABLE_MEMORY)
  add_executable(xenon_memory_v2_benchmarks
    memory_benchmarks.cpp)
  target_link_libraries(xenon_memory_v2_benchmarks PRIVATE xenon_memory)
  target_compile_definitions(xenon_memory_v2_benchmarks PRIVATE
    XENON_BENCHMARK_BUILD_CONFIG="$<CONFIG>")
  xenon_enable_test_asserts(xenon_memory_v2_benchmarks)
endif()

if(XENON_BUILD_TESTS)

  if(XENON_ENABLE_MEMORY)
    add_executable(xenon_memory_tests memory_tests.cpp)
    target_link_libraries(xenon_memory_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_memory_tests)
    add_test(NAME xenon_memory_tests COMMAND xenon_memory_tests)

    add_executable(xenon_memory_hardening_tests
      memory_hardening_tests.cpp)
    target_link_libraries(xenon_memory_hardening_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_memory_hardening_tests)
    add_test(NAME xenon_memory_hardening_tests
      COMMAND xenon_memory_hardening_tests)

    add_executable(xenon_memory_ordering_tests
      memory_ordering_tests.cpp)
    target_link_libraries(xenon_memory_ordering_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_memory_ordering_tests)
    add_test(NAME xenon_memory_ordering_tests COMMAND xenon_memory_ordering_tests)

    add_executable(xenon_memory_type_tests
      memory_type_tests.cpp)
    target_link_libraries(xenon_memory_type_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_memory_type_tests)
    add_test(NAME xenon_memory_type_tests COMMAND xenon_memory_type_tests)

    add_executable(xenon_mmio_tests
      mmio_tests.cpp)
    target_link_libraries(xenon_mmio_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_mmio_tests)
    add_test(NAME xenon_mmio_tests COMMAND xenon_mmio_tests)

    add_executable(xenon_memory_fault_tests
      fault_tests.cpp)
    target_link_libraries(xenon_memory_fault_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_memory_fault_tests)
    add_test(NAME xenon_memory_fault_tests COMMAND xenon_memory_fault_tests)

    add_executable(xenon_executable_code_cache_tests
      executable_code_cache_tests.cpp)
    target_link_libraries(xenon_executable_code_cache_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_executable_code_cache_tests)
    add_test(NAME xenon_executable_code_cache_tests
      COMMAND xenon_executable_code_cache_tests)

    add_executable(xenon_host_vm_tests host_vm_tests.cpp)
    target_link_libraries(xenon_host_vm_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_host_vm_tests)
    add_test(NAME xenon_host_vm_tests COMMAND xenon_host_vm_tests)

    add_executable(xenon_gpu_coherency_tests
      gpu_coherency_tests.cpp)
    target_link_libraries(xenon_gpu_coherency_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_gpu_coherency_tests)
    add_test(NAME xenon_gpu_coherency_tests COMMAND xenon_gpu_coherency_tests)

    add_executable(xenon_memory_codegen_integration codegen_memory_integration.cpp)
    target_link_libraries(xenon_memory_codegen_integration PRIVATE xenon_cpu)
    set(XENON_GENERATED_MEMORY_INTEGRATION
      "${CMAKE_CURRENT_BINARY_DIR}/generated_memory_integration.cpp")
    add_custom_command(
      OUTPUT "${XENON_GENERATED_MEMORY_INTEGRATION}"
      COMMAND xenon_memory_codegen_integration "${XENON_GENERATED_MEMORY_INTEGRATION}"
      DEPENDS xenon_memory_codegen_integration
      VERBATIM)
    add_executable(xenon_memory_cpu_integration
      memory_cpu_integration.cpp
      "${XENON_GENERATED_MEMORY_INTEGRATION}")
    target_link_libraries(xenon_memory_cpu_integration PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_memory_cpu_integration)
    add_test(NAME xenon_memory_cpu_integration COMMAND xenon_memory_cpu_integration)
  endif()
endif()
