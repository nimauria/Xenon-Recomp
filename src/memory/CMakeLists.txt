# -----------------------------------------------------------------------------
# Production guest memory. FlatMemory above remains a CPU test fixture only.
if(XENON_ENABLE_MEMORY)
  if(WIN32)
    set(XENON_HOST_VM_SOURCE mapping/host_vm_windows.cpp)
  elseif(UNIX)
    set(XENON_HOST_VM_SOURCE mapping/host_vm_posix.cpp)
  else()
    set(XENON_HOST_VM_SOURCE mapping/host_vm_fallback.cpp)
  endif()
  add_library(xenon_memory STATIC
    # AddressSpace: construction, page table, virtual and physical
    # allocation, the guest access path, reservations and diagnostics.
    guest/address_space.cpp
    guest/page_table/hot_page_table.cpp
    guest/virtual/virtual_allocation.cpp
    guest/virtual/virtual_mapping.cpp
    guest/physical/physical_allocation.cpp
    guest/physical/physical_aliases.cpp
    guest/physical/physical_writes.cpp
    guest/access/guest_access.cpp
    guest/access/mmio_registration.cpp
    guest/reservations/reservations.cpp
    guest/coherency/memory_ordering.cpp
    guest/diagnostics/fault_reporting.cpp
    guest/diagnostics/invariants.cpp
    ${XENON_HOST_VM_SOURCE}
  )
  target_link_libraries(xenon_memory PUBLIC xenon_cpu PRIVATE xenon_logging)
  target_include_directories(xenon_memory
    PUBLIC "${XENON_SOURCE_DIR}/include"
    PRIVATE "${XENON_SOURCE_DIR}/src")
  target_compile_features(xenon_memory PUBLIC cxx_std_20)
  target_compile_definitions(xenon_memory PUBLIC
    XENON_MEMORY_DEFAULT_DIRECT_APERTURE=$<BOOL:${XENON_MEMORY_DEFAULT_DIRECT_APERTURE}>)
  xenon_enable_warnings(xenon_memory)
  add_library(Xenon::Memory ALIAS xenon_memory)
endif()
