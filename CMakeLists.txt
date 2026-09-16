cmake_minimum_required(VERSION 3.25)

project(
  XenonRecomp
  VERSION 0.1.0
  LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(Platforms)
include(Architectures)
include(Warnings)
include(Dependencies)

option(XENON_BUILD_TESTS "Build Xenon Recomp tests" ON)
option(XENON_ENABLE_MEMORY "Build the production Xenon memory subsystem" ON)
option(XENON_ENABLE_GRAPHICS "Build the Xenos graphics frontend" ON)
option(XENON_ENABLE_AUDIO "Enable audio subsystem" OFF)
option(XENON_ENABLE_INPUT "Enable input subsystem" OFF)
option(XENON_ENABLE_NETWORK "Enable networking subsystem" OFF)

if(XENON_ENABLE_GRAPHICS AND NOT XENON_ENABLE_MEMORY)
  message(FATAL_ERROR "XENON_ENABLE_GRAPHICS requires XENON_ENABLE_MEMORY")
endif()

# -----------------------------------------------------------------------------
# Core runtime
add_library(xenon_core STATIC
  src/core/runtime.cpp
)
target_include_directories(xenon_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(xenon_core PUBLIC cxx_std_20)
xenon_enable_warnings(xenon_core)
add_library(Xenon::Core ALIAS xenon_core)

# -----------------------------------------------------------------------------
# CPU: Xenon PPC/VMX128 frontend -> Xenon IR -> optimizer -> native C++ AOT.
add_library(xenon_cpu STATIC
  src/cpu/ppc/state.cpp
  src/cpu/ppc/decoder.cpp
  src/cpu/ppc/lifter.cpp
  src/cpu/ppc/lifter_integer.cpp
  src/cpu/ppc/lifter_control.cpp
  src/cpu/ppc/lifter_memory.cpp
  src/cpu/ppc/lifter_fpu.cpp
  src/cpu/ppc/lifter_vector.cpp
  src/cpu/ppc/flat_memory.cpp
  src/cpu/ir/ir.cpp
  src/cpu/optimizer/optimizer.cpp
  src/cpu/codegen/aot_semantics.cpp
  src/cpu/codegen/backend_cpp_aot.cpp
  src/cpu/codegen/function_compiler.cpp
)
target_include_directories(xenon_cpu
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/ppc"
)
target_compile_features(xenon_cpu PUBLIC cxx_std_20)
xenon_enable_guest_fp_semantics(xenon_cpu PUBLIC)
xenon_enable_warnings(xenon_cpu)
add_library(Xenon::CPU ALIAS xenon_cpu)

# -----------------------------------------------------------------------------
# Production guest memory. FlatMemory above remains a CPU test fixture only.
if(XENON_ENABLE_MEMORY)
  if(WIN32)
    message(FATAL_ERROR
      "The production AddressSpace backing is currently Linux/POSIX-only. "
      "Configure Windows CPU-only builds with -DXENON_ENABLE_MEMORY=OFF "
      "-DXENON_ENABLE_GRAPHICS=OFF until the Windows host-memory provider lands.")
  endif()

  add_library(xenon_memory STATIC
    src/memory/guest/address_space.cpp
  )
  target_link_libraries(xenon_memory PUBLIC xenon_cpu)
  target_include_directories(xenon_memory PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_compile_features(xenon_memory PUBLIC cxx_std_20)
  xenon_enable_warnings(xenon_memory)
  add_library(Xenon::Memory ALIAS xenon_memory)
endif()

# -----------------------------------------------------------------------------
# Graphics: Xenos PM4/shader/EDRAM frontend. Vulkan and D3D12 remain backends.
if(XENON_ENABLE_GRAPHICS)
  add_library(xenon_graphics STATIC
    src/graphics/xenos/register_file.cpp
    src/graphics/xenos/shader.cpp
    src/graphics/xenos/edram.cpp
    src/graphics/xenos/command_processor.cpp
    src/graphics/xenos/graphics_system.cpp
  )
  target_link_libraries(xenon_graphics PUBLIC xenon_memory)
  target_include_directories(xenon_graphics PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_compile_features(xenon_graphics PUBLIC cxx_std_20)
  xenon_enable_warnings(xenon_graphics)
  add_library(Xenon::Graphics ALIAS xenon_graphics)
  add_library(Xenon::GPU ALIAS xenon_graphics)
  # Compatibility with the phase-1 working tree target name.
  add_library(xenon_gpu ALIAS xenon_graphics)
endif()

# -----------------------------------------------------------------------------
# Tests
if(XENON_BUILD_TESTS)
  enable_testing()

  add_executable(xenon_cpu_tests tests/cpu/ppc/cpu_tests.cpp)
  target_link_libraries(xenon_cpu_tests PRIVATE xenon_cpu)
  target_include_directories(xenon_cpu_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/cpu/ppc")
  xenon_enable_test_asserts(xenon_cpu_tests)
  add_test(NAME xenon_cpu_tests COMMAND xenon_cpu_tests)

  # Generate and compile one function for every canonical instruction.
  add_executable(xenon_cpu_codegen_all tests/cpu/codegen/codegen_all.cpp)
  target_link_libraries(xenon_cpu_codegen_all PRIVATE xenon_cpu)
  set(XENON_GENERATED_ALL "${CMAKE_CURRENT_BINARY_DIR}/generated_all_opcodes.cpp")
  add_custom_command(
    OUTPUT "${XENON_GENERATED_ALL}"
    COMMAND xenon_cpu_codegen_all "${XENON_GENERATED_ALL}"
    DEPENDS xenon_cpu_codegen_all
    VERBATIM)
  add_library(xenon_cpu_generated_compile OBJECT "${XENON_GENERATED_ALL}")
  target_include_directories(xenon_cpu_generated_compile PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_compile_features(xenon_cpu_generated_compile PRIVATE cxx_std_20)
  xenon_enable_guest_fp_semantics(xenon_cpu_generated_compile PRIVATE)
  add_dependencies(xenon_cpu_generated_compile xenon_cpu_codegen_all)

  add_executable(xenon_cpu_codegen_smoke tests/cpu/codegen/codegen_smoke.cpp)
  target_link_libraries(xenon_cpu_codegen_smoke PRIVATE xenon_cpu)
  set(XENON_GENERATED_SMOKE "${CMAKE_CURRENT_BINARY_DIR}/generated_native_smoke.cpp")
  add_custom_command(
    OUTPUT "${XENON_GENERATED_SMOKE}"
    COMMAND xenon_cpu_codegen_smoke "${XENON_GENERATED_SMOKE}"
    DEPENDS xenon_cpu_codegen_smoke
    VERBATIM)
  add_executable(xenon_cpu_native_smoke
    tests/cpu/x86_64/native_smoke.cpp
    "${XENON_GENERATED_SMOKE}")
  target_link_libraries(xenon_cpu_native_smoke PRIVATE xenon_cpu)
  xenon_enable_test_asserts(xenon_cpu_native_smoke)
  add_test(NAME xenon_cpu_native_smoke COMMAND xenon_cpu_native_smoke)

  add_executable(xenon_cpu_exhaustive_native
    tests/cpu/x86_64/exhaustive_native.cpp
    $<TARGET_OBJECTS:xenon_cpu_generated_compile>)
  target_link_libraries(xenon_cpu_exhaustive_native PRIVATE xenon_cpu)
  add_dependencies(xenon_cpu_exhaustive_native xenon_cpu_generated_compile)
  xenon_enable_test_asserts(xenon_cpu_exhaustive_native)
  add_test(NAME xenon_cpu_exhaustive_native COMMAND xenon_cpu_exhaustive_native)

  function(xenon_add_generated_cpu_test name generator source)
    add_executable(xenon_cpu_codegen_${name} "tests/cpu/codegen/${generator}")
    target_link_libraries(xenon_cpu_codegen_${name} PRIVATE xenon_cpu)
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated_${name}.cpp")
    add_custom_command(
      OUTPUT "${generated}"
      COMMAND xenon_cpu_codegen_${name} "${generated}"
      DEPENDS xenon_cpu_codegen_${name}
      VERBATIM)
    add_executable(xenon_cpu_${name} "tests/cpu/x86_64/${source}" "${generated}")
    target_link_libraries(xenon_cpu_${name} PRIVATE xenon_cpu)
    xenon_enable_test_asserts(xenon_cpu_${name})
    add_test(NAME xenon_cpu_${name} COMMAND xenon_cpu_${name})
  endfunction()

  xenon_add_generated_cpu_test(function_flow codegen_function_flow.cpp function_flow.cpp)
  xenon_add_generated_cpu_test(branch_matrix codegen_branch_matrix.cpp branch_matrix.cpp)
  xenon_add_generated_cpu_test(control_boundaries codegen_control_boundaries.cpp control_boundaries.cpp)
  xenon_add_generated_cpu_test(vector_memory codegen_vector_memory.cpp vector_memory.cpp)
  xenon_add_generated_cpu_test(indirect_branch codegen_indirect_branch.cpp indirect_branch.cpp)
  xenon_add_generated_cpu_test(scalar_memory codegen_scalar_memory.cpp scalar_memory.cpp)
  xenon_add_generated_cpu_test(integer_edges codegen_integer_edges.cpp integer_edges.cpp)

  if(XENON_ENABLE_MEMORY)
    add_executable(xenon_memory_tests tests/memory/memory_tests.cpp)
    target_link_libraries(xenon_memory_tests PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_memory_tests)
    add_test(NAME xenon_memory_tests COMMAND xenon_memory_tests)

    add_executable(xenon_memory_codegen_integration tests/memory/codegen_memory_integration.cpp)
    target_link_libraries(xenon_memory_codegen_integration PRIVATE xenon_cpu)
    set(XENON_GENERATED_MEMORY_INTEGRATION
      "${CMAKE_CURRENT_BINARY_DIR}/generated_memory_integration.cpp")
    add_custom_command(
      OUTPUT "${XENON_GENERATED_MEMORY_INTEGRATION}"
      COMMAND xenon_memory_codegen_integration "${XENON_GENERATED_MEMORY_INTEGRATION}"
      DEPENDS xenon_memory_codegen_integration
      VERBATIM)
    add_executable(xenon_memory_cpu_integration
      tests/memory/memory_cpu_integration.cpp
      "${XENON_GENERATED_MEMORY_INTEGRATION}")
    target_link_libraries(xenon_memory_cpu_integration PRIVATE xenon_memory)
    xenon_enable_test_asserts(xenon_memory_cpu_integration)
    add_test(NAME xenon_memory_cpu_integration COMMAND xenon_memory_cpu_integration)
  endif()

  if(XENON_ENABLE_GRAPHICS)
    add_executable(xenon_gpu_frontend_tests tests/graphics/xenos/gpu_frontend.cpp)
    target_link_libraries(xenon_gpu_frontend_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_gpu_frontend_tests)
    add_test(NAME xenon_gpu_frontend_tests COMMAND xenon_gpu_frontend_tests)
  endif()
endif()
