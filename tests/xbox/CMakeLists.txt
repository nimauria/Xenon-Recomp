if(XENON_BUILD_TESTS)

  # XEX format tests link only the XEX libraries, which keeps them proof that
  # XEX parsing, decryption and decompression need no guest kernel.
  if(TARGET xenon_xex)
    add_executable(xenon_xex_crypto_tests xex_crypto_tests.cpp)
    target_link_libraries(xenon_xex_crypto_tests PRIVATE xenon_xex)
    xenon_enable_test_asserts(xenon_xex_crypto_tests)
    add_test(NAME xenon_xex_crypto_tests COMMAND xenon_xex_crypto_tests)
    add_executable(xenon_xex_lzx_tests xex_lzx_tests.cpp)
    target_link_libraries(xenon_xex_lzx_tests PRIVATE xenon_xex)
    xenon_enable_test_asserts(xenon_xex_lzx_tests)
    add_test(NAME xenon_xex_lzx_tests COMMAND xenon_xex_lzx_tests)
  endif()
  if(TARGET xenon_xex_loading)
    add_executable(xenon_xex_loader_tests xex_loader_tests.cpp)
    target_link_libraries(xenon_xex_loader_tests PRIVATE xenon_xex_loading)
    xenon_enable_test_asserts(xenon_xex_loader_tests)
    add_test(NAME xenon_xex_loader_tests COMMAND xenon_xex_loader_tests)
  endif()

  if(TARGET xenon_xbox_kernel_io)
    add_executable(xenon_xbox_import_tests xbox_import_tests.cpp)
    target_link_libraries(xenon_xbox_import_tests PRIVATE xenon_xbox_kernel_io)
    xenon_enable_test_asserts(xenon_xbox_import_tests)
    add_test(NAME xenon_xbox_import_tests COMMAND xenon_xbox_import_tests)
    add_executable(xenon_rtl_tests rtl_tests.cpp)
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
    add_executable(xenon_xboxkrnl_time_export_tests time_export_tests.cpp)
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
    add_executable(xenon_xboxkrnl_memory_export_tests memory_export_tests.cpp)
    target_link_libraries(xenon_xboxkrnl_memory_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_xboxkrnl_memory_export_tests)
    add_test(NAME xenon_xboxkrnl_memory_export_tests COMMAND xenon_xboxkrnl_memory_export_tests)
  endif()

  # xboxkrnl process-type exports (KeGetCurrentProcessType,
  # KeSetCurrentProcessType) - the next real export Ace Combat 6's boot path
  # calls once the memory-management exports above were fixed.
  if(TARGET xenon_core)
    add_executable(xenon_xboxkrnl_process_export_tests process_export_tests.cpp)
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
      rtl_critical_section_export_tests.cpp)
    target_link_libraries(xenon_rtl_critical_section_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_rtl_critical_section_export_tests)
    add_test(NAME xenon_rtl_critical_section_export_tests
             COMMAND xenon_rtl_critical_section_export_tests)
  endif()

  # xboxkrnl XexCheckExecutablePrivilege - the next real export Ace Combat
  # 6's boot path calls once the gpr[12]-vs-gpr[0] save/restore-helper
  # LR-source bug was fixed.
  if(TARGET xenon_core)
    add_executable(xenon_xex_module_export_tests xex_module_export_tests.cpp)
    target_link_libraries(xenon_xex_module_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_xex_module_export_tests)
    add_test(NAME xenon_xex_module_export_tests COMMAND xenon_xex_module_export_tests)
  endif()

  # xboxkrnl KeTls* exports - the next real export Ace Combat 6's boot path
  # calls once XexCheckExecutablePrivilege above was fixed.
  if(TARGET xenon_core)
    add_executable(xenon_tls_export_tests tls_export_tests.cpp)
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
    add_executable(xenon_video_export_tests video_export_tests.cpp)
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
    add_executable(xenon_xboxkrnl_sync_export_tests sync_export_tests.cpp)
    target_link_libraries(xenon_xboxkrnl_sync_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_xboxkrnl_sync_export_tests)
    add_test(NAME xenon_xboxkrnl_sync_export_tests COMMAND xenon_xboxkrnl_sync_export_tests)
    add_executable(xenon_xboxkrnl_ke_sync_export_tests ke_sync_export_tests.cpp)
    target_link_libraries(xenon_xboxkrnl_ke_sync_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_xboxkrnl_ke_sync_export_tests)
    add_test(NAME xenon_xboxkrnl_ke_sync_export_tests COMMAND xenon_xboxkrnl_ke_sync_export_tests)
    add_executable(xenon_ke_irql_export_tests ke_irql_export_tests.cpp)
    target_link_libraries(xenon_ke_irql_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_ke_irql_export_tests)
    add_test(NAME xenon_ke_irql_export_tests COMMAND xenon_ke_irql_export_tests)
    add_executable(xenon_rtl_string_memory_export_tests
      rtl_string_memory_export_tests.cpp)
    target_link_libraries(xenon_rtl_string_memory_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_rtl_string_memory_export_tests)
    add_test(NAME xenon_rtl_string_memory_export_tests
             COMMAND xenon_rtl_string_memory_export_tests)
    add_executable(xenon_misc_export_tests misc_export_tests.cpp)
    target_link_libraries(xenon_misc_export_tests PRIVATE xenon_core)
    xenon_enable_test_asserts(xenon_misc_export_tests)
    add_test(NAME xenon_misc_export_tests COMMAND xenon_misc_export_tests)
  endif()
  foreach(test_name IN ITEMS module_lookup ob_exports pool_debug
      rtl_string_family string_format threading_exports virtual_memory_export)
    xenon_add_standalone_test(xenon_xbox_${test_name}_tests
      ${test_name}_tests.cpp xenon_core)
  endforeach()
endif()
