if(XENON_BUILD_TESTS)

  # XAM tests
  add_executable(xenon_xam_user_manager_tests user_manager_tests.cpp)
  target_link_libraries(xenon_xam_user_manager_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_xam_user_manager_tests)
  add_test(NAME xenon_xam_user_manager_tests COMMAND xenon_xam_user_manager_tests)

  add_executable(xenon_xam_export_ordinal_tests xam_export_ordinal_tests.cpp)
  target_link_libraries(xenon_xam_export_ordinal_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_xam_export_ordinal_tests)
  add_test(NAME xenon_xam_export_ordinal_tests COMMAND xenon_xam_export_ordinal_tests)

  add_executable(xenon_guest_enumerator_tests guest_enumerator_tests.cpp)
  target_link_libraries(xenon_guest_enumerator_tests PRIVATE xenon_core)
  xenon_enable_test_asserts(xenon_guest_enumerator_tests)
  add_test(NAME xenon_guest_enumerator_tests COMMAND xenon_guest_enumerator_tests)
  if(WIN32 AND TARGET xenon_memory)
    xenon_add_standalone_test(xenon_xam_net_socket_tests
      xam_net_socket_tests.cpp xenon_core)
  endif()

  xenon_add_standalone_test(xenon_xam_content_services_tests
    content_services_tests.cpp xenon_core)
endif()
