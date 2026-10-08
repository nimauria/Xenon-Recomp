# -----------------------------------------------------------------------------
# Host-side kernel object/I/O layer. This still has no guest Memory dependency.
if(XENON_ENABLE_KERNEL)
  add_library(xenon_kernel STATIC
    handles/object.cpp
    handles/handle_table.cpp
    handles/io_request.cpp
    handles/file_object.cpp
    handles/io_manager.cpp
    xboxkrnl/xbox_io.cpp
    synchronization/completion_port.cpp
    synchronization/event.cpp
    synchronization/semaphore.cpp
    synchronization/mutant.cpp
    synchronization/timer.cpp
    synchronization/timer_manager.cpp
    synchronization/wait.cpp
    threading/thread.cpp
    timing/time.cpp
    module/module.cpp
    exception/exception.cpp
  )
  target_link_libraries(xenon_kernel PUBLIC xenon_filesystem xenon_logging)
  target_include_directories(xenon_kernel PUBLIC "${XENON_SOURCE_DIR}/include")
  target_compile_features(xenon_kernel PUBLIC cxx_std_20)
  xenon_enable_warnings(xenon_kernel)
  add_library(Xenon::Kernel ALIAS xenon_kernel)

  # Kernel memory integration requires Memory V2
  if(XENON_ENABLE_MEMORY)
    target_sources(xenon_kernel PRIVATE
      memory/memory.cpp
      memory/heap.cpp
      memory/pool.cpp
      process/process.cpp
    )
    target_link_libraries(xenon_kernel PUBLIC xenon_memory)
  endif()
endif()
