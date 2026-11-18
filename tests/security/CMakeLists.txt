# Input hardening (XEN-008): deterministic mutation fuzzing of every parser
# that reads untrusted input. Run it under the sanitizer presets as well.
if(XENON_BUILD_TESTS AND TARGET xenon_xex_loading AND TARGET xenon_filesystem AND TARGET xenon_graphics)
  add_executable(xenon_input_fuzz_tests input_fuzz_tests.cpp)
  target_link_libraries(xenon_input_fuzz_tests PRIVATE
    xenon_xex_loading xenon_xex xenon_filesystem xenon_graphics xenon_core)
  if(TARGET xenon_input)
    target_link_libraries(xenon_input_fuzz_tests PRIVATE xenon_input)
    target_compile_definitions(xenon_input_fuzz_tests PRIVATE XENON_FUZZ_HAS_INPUT=1)
  endif()
  if(TARGET xenon_recomp)
    target_link_libraries(xenon_input_fuzz_tests PRIVATE xenon_recomp)
    target_compile_definitions(xenon_input_fuzz_tests PRIVATE XENON_FUZZ_HAS_RECOMP=1)
  endif()
  # The runtime host's launch configuration parser (only needs xenon_core).
  target_sources(xenon_input_fuzz_tests PRIVATE ${XENON_SOURCE_DIR}/runtime_host/src/launch_config.cpp)
  target_include_directories(xenon_input_fuzz_tests PRIVATE ${XENON_SOURCE_DIR}/runtime_host/src)
  target_compile_definitions(xenon_input_fuzz_tests PRIVATE XENON_FUZZ_HAS_LAUNCH_CONFIG=1)
  xenon_enable_test_asserts(xenon_input_fuzz_tests)
  add_test(NAME xenon_input_fuzz_tests COMMAND xenon_input_fuzz_tests)
  set_tests_properties(xenon_input_fuzz_tests PROPERTIES TIMEOUT 1800)
endif()
