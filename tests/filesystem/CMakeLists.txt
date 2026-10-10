if(XENON_BUILD_TESTS)

  if(XENON_ENABLE_FILESYSTEM)
    add_executable(xenon_filesystem_tests filesystem_tests.cpp)
    target_link_libraries(xenon_filesystem_tests PRIVATE xenon_filesystem)
    xenon_enable_test_asserts(xenon_filesystem_tests)
    add_test(NAME xenon_filesystem_tests COMMAND xenon_filesystem_tests)
  endif()
endif()
