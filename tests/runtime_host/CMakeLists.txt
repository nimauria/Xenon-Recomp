if(XENON_BUILD_TESTS AND TARGET xenon_runtime_host)
  add_executable(xenon_runtime_host_tests
    launch_config_tests.cpp
    ${XENON_SOURCE_DIR}/runtime_host/src/launch_config.cpp
    ${XENON_SOURCE_DIR}/runtime_host/src/launch_config.hpp
    ${XENON_SOURCE_DIR}/runtime_host/src/host_setup.cpp
    ${XENON_SOURCE_DIR}/runtime_host/src/host_setup.hpp
  )
  target_include_directories(xenon_runtime_host_tests PRIVATE ${XENON_SOURCE_DIR}/runtime_host/src)
  target_link_libraries(xenon_runtime_host_tests PRIVATE xenon_core)
  target_compile_features(xenon_runtime_host_tests PRIVATE cxx_std_20)
  xenon_enable_test_asserts(xenon_runtime_host_tests)
  add_test(NAME xenon_runtime_host_tests COMMAND xenon_runtime_host_tests)
  add_executable(xenon_runtime_content_source_tests
    content_source_tests.cpp
    ${XENON_SOURCE_DIR}/runtime_host/src/content_source.cpp
    ${XENON_SOURCE_DIR}/runtime_host/src/content_source.hpp
  )
  target_include_directories(xenon_runtime_content_source_tests PRIVATE ${XENON_SOURCE_DIR}/runtime_host/src)
  target_link_libraries(xenon_runtime_content_source_tests PRIVATE xenon_core)
  target_compile_features(xenon_runtime_content_source_tests PRIVATE cxx_std_20)
  xenon_enable_test_asserts(xenon_runtime_content_source_tests)
  add_test(NAME xenon_runtime_content_source_tests COMMAND xenon_runtime_content_source_tests)
  add_executable(xenon_status_writer_tests
    status_writer_tests.cpp
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
  if(WIN32 AND XENON_RUNTIME_HOST_STACK_RESERVE)
    add_executable(xenon_runtime_host_stack_tests host_stack_tests.cpp)
    xenon_enable_test_asserts(xenon_runtime_host_stack_tests)
    add_dependencies(xenon_runtime_host_stack_tests xenon_runtime_host)
    add_test(NAME xenon_runtime_host_stack_tests
      COMMAND xenon_runtime_host_stack_tests $<TARGET_FILE:xenon_runtime_host>
              ${XENON_RUNTIME_HOST_STACK_RESERVE})
  endif()
  xenon_resolve_sdl3(XENON_PRESENTATION_HOST_TESTS_SDL_TARGET)
  if(NOT XENON_PRESENTATION_HOST_TESTS_SDL_TARGET)
    message(FATAL_ERROR
      "xenon_presentation_host_tests requires SDL3 (needed by presentation_host.cpp) - "
      "resolve an SDL3 dependency instead of silently skipping this test")
  endif()
  add_executable(xenon_presentation_host_tests
    presentation_host_tests.cpp
    ${XENON_SOURCE_DIR}/runtime_host/src/presentation_host.cpp
    ${XENON_SOURCE_DIR}/runtime_host/src/presentation_host.hpp
  )
  target_include_directories(xenon_presentation_host_tests PRIVATE ${XENON_SOURCE_DIR}/runtime_host/src)
  target_link_libraries(xenon_presentation_host_tests PRIVATE xenon_core ${XENON_PRESENTATION_HOST_TESTS_SDL_TARGET})
  target_compile_features(xenon_presentation_host_tests PRIVATE cxx_std_20)
  xenon_enable_test_asserts(xenon_presentation_host_tests)
  add_test(NAME xenon_presentation_host_tests COMMAND xenon_presentation_host_tests)
endif()
