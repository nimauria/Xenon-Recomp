# Tests and benchmarks. Executables and test modules stay at the build root
# so existing scripts and documentation paths keep working.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")

if(XENON_BUILD_TESTS)
  function(xenon_add_standalone_test target source)
    add_executable(${target} "${source}")
    target_link_libraries(${target} PRIVATE ${ARGN})
    xenon_enable_test_asserts(${target})
    add_test(NAME ${target} COMMAND ${target})
  endfunction()

  # Tests that configure and build a generated project use this build's
  # generator and C++ compiler for it (tests/support/nested_cmake.hpp).
  function(xenon_test_uses_nested_cmake target)
    target_compile_definitions(${target} PRIVATE
      XENON_TEST_CMAKE_GENERATOR="${CMAKE_GENERATOR}"
      XENON_TEST_CMAKE_GENERATOR_PLATFORM="${CMAKE_GENERATOR_PLATFORM}"
      XENON_TEST_CMAKE_MAKE_PROGRAM="${CMAKE_MAKE_PROGRAM}"
      XENON_TEST_CXX_COMPILER="${CMAKE_CXX_COMPILER}")
  endfunction()

  find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
endif()

add_subdirectory(audio)
add_subdirectory(core)
add_subdirectory(cpu)
add_subdirectory(deps)
add_subdirectory(filesystem)
add_subdirectory(graphics)
add_subdirectory(input)
add_subdirectory(kernel)
add_subdirectory(logging)
add_subdirectory(memory)
add_subdirectory(network)
add_subdirectory(recomp)
add_subdirectory(release)
add_subdirectory(runtime_host)
add_subdirectory(xam)
add_subdirectory(xbox)
