if(XENON_BUILD_TESTS)

  if(Python3_Interpreter_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/verify_install_tests.py")
    add_test(NAME xenon_release_verify_install_tests
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/verify_install_tests.py")
    set_tests_properties(xenon_release_verify_install_tests PROPERTIES
      WORKING_DIRECTORY "${XENON_SOURCE_DIR}")
  endif()
endif()
