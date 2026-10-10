if(XENON_BUILD_TESTS)

  if(TARGET xenon_network)
    add_executable(xenon_network_tests network_tests.cpp)
    target_link_libraries(xenon_network_tests PRIVATE Xenon::Network)
    target_compile_definitions(xenon_network_tests PRIVATE
      XENON_NETWORK_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    xenon_enable_test_asserts(xenon_network_tests)
    add_test(NAME xenon_network_tests COMMAND xenon_network_tests)
  endif()
endif()
