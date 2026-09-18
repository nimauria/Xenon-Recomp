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
option(XENON_BUILD_LAUNCHER "Build the Xenon launcher UI when Qt 6 is available" ON)
option(XENON_ENABLE_MEMORY "Build the production Xenon memory subsystem" ON)
option(XENON_ENABLE_GRAPHICS "Build the Xenos graphics frontend" ON)
option(XENON_ENABLE_VULKAN "Build the Vulkan backend when the SDK is available" ON)
option(XENON_ENABLE_D3D12 "Build the Direct3D 12 backend on Windows" ON)
option(XENON_ENABLE_DXC "Build the HLSL/DXC shader compiler when DXC is available" ON)
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
    set(XENON_HOST_VM_SOURCE src/memory/mapping/host_vm_windows.cpp)
  elseif(UNIX)
    set(XENON_HOST_VM_SOURCE src/memory/mapping/host_vm_posix.cpp)
  else()
    set(XENON_HOST_VM_SOURCE src/memory/mapping/host_vm_fallback.cpp)
  endif()
  add_library(xenon_memory STATIC
    src/memory/guest/address_space.cpp
    ${XENON_HOST_VM_SOURCE}
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
    src/graphics/xenos/shader_decoder.cpp
    src/graphics/xenos/shader_translation.cpp
    src/graphics/xenos/depth_format.cpp
    src/graphics/xenos/resource_ir.cpp
    src/graphics/xenos/resource_barrier.cpp
    src/graphics/xenos/texture.cpp
    src/graphics/xenos/edram.cpp
    src/graphics/xenos/edram_surface.cpp
    src/graphics/xenos/edram_ownership.cpp
    src/graphics/xenos/primitive_processor.cpp
    src/graphics/xenos/command_processor.cpp
    src/graphics/xenos/graphics_system.cpp
    src/graphics/windows/backend_capabilities.cpp
  )
  target_link_libraries(xenon_graphics PUBLIC xenon_memory)
  if(WIN32)
    # Runtime capability discovery lives in the common layer and calls DXGI
    # and D3D12 even when the optional renderer target is disabled.
    target_link_libraries(xenon_graphics PRIVATE d3d12 dxgi)
  endif()
  target_include_directories(xenon_graphics PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_compile_features(xenon_graphics PUBLIC cxx_std_20)
  xenon_enable_warnings(xenon_graphics)
  add_library(Xenon::Graphics ALIAS xenon_graphics)
  add_library(Xenon::GPU ALIAS xenon_graphics)
  # Compatibility with the phase-1 working tree target name.
  add_library(xenon_gpu ALIAS xenon_graphics)

  if(WIN32 AND XENON_ENABLE_DXC)
    # The Windows SDK DXC can emit DXIL only. Prefer the Vulkan SDK's matched
    # headers/import library/runtime because that build includes SPIR-V codegen.
    if(DEFINED ENV{VULKAN_SDK})
      find_path(XENON_DXC_INCLUDE_DIR dxc/dxcapi.h
        PATHS "$ENV{VULKAN_SDK}/Include" NO_DEFAULT_PATH)
      find_library(XENON_DXC_LIBRARY NAMES dxcompiler
        PATHS "$ENV{VULKAN_SDK}/Lib" NO_DEFAULT_PATH)
      find_file(XENON_DXC_RUNTIME NAMES dxcompiler.dll
        PATHS "$ENV{VULKAN_SDK}/Bin" NO_DEFAULT_PATH)
    endif()
    find_path(XENON_DXC_INCLUDE_DIR dxc/dxcapi.h)
    find_library(XENON_DXC_LIBRARY NAMES dxcompiler)
    find_file(XENON_DXC_RUNTIME NAMES dxcompiler.dll)
    if(XENON_DXC_INCLUDE_DIR AND XENON_DXC_LIBRARY)
      add_library(xenon_graphics_dxc STATIC
        src/graphics/dxc/shader_compiler.cpp)
      target_include_directories(xenon_graphics_dxc
        PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
        PRIVATE "${XENON_DXC_INCLUDE_DIR}")
      target_link_libraries(xenon_graphics_dxc PUBLIC xenon_graphics
        PRIVATE "${XENON_DXC_LIBRARY}")
      target_compile_features(xenon_graphics_dxc PUBLIC cxx_std_20)
      xenon_enable_warnings(xenon_graphics_dxc)
      add_library(Xenon::GraphicsDXC ALIAS xenon_graphics_dxc)
      message(STATUS "DXC shader compiler enabled: ${XENON_DXC_LIBRARY}")
    else()
      message(STATUS "DXC shader compiler disabled: dxcapi.h or dxcompiler library not found")
    endif()
  endif()

  if(XENON_ENABLE_VULKAN)
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
      target_compile_definitions(xenon_graphics PRIVATE XENON_HAS_VULKAN_SDK=1)
      add_library(xenon_graphics_vulkan STATIC
        src/graphics/vulkan/context.cpp
        src/graphics/vulkan/command_queue.cpp
        src/graphics/vulkan/memory.cpp
        src/graphics/vulkan/guest_memory_mirror.cpp
        src/graphics/vulkan/resource_layout.cpp
        src/graphics/vulkan/pipeline.cpp
        src/graphics/vulkan/render_target.cpp
        src/graphics/vulkan/depth_target.cpp
        src/graphics/vulkan/texture.cpp
        src/graphics/vulkan/backend.cpp)
      target_include_directories(xenon_graphics_vulkan PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
      target_link_libraries(xenon_graphics_vulkan PUBLIC xenon_graphics Vulkan::Vulkan)
      if(TARGET xenon_graphics_dxc)
        target_link_libraries(xenon_graphics_vulkan PRIVATE xenon_graphics_dxc)
        target_compile_definitions(xenon_graphics_vulkan PRIVATE XENON_HAS_DXC=1)
      endif()
      target_compile_features(xenon_graphics_vulkan PUBLIC cxx_std_20)
      xenon_enable_warnings(xenon_graphics_vulkan)
      add_library(Xenon::GraphicsVulkan ALIAS xenon_graphics_vulkan)
    else()
      message(STATUS "Vulkan runtime backend disabled: install the Vulkan SDK to build it")
    endif()
  endif()

  if(WIN32 AND XENON_ENABLE_D3D12)
    add_library(xenon_graphics_d3d12 STATIC
      src/graphics/d3d12/context.cpp
      src/graphics/d3d12/command_queue.cpp
      src/graphics/d3d12/memory.cpp
      src/graphics/d3d12/guest_memory_mirror.cpp
      src/graphics/d3d12/resource_layout.cpp
      src/graphics/d3d12/pipeline.cpp
      src/graphics/d3d12/render_target.cpp
      src/graphics/d3d12/depth_target.cpp
      src/graphics/d3d12/texture.cpp
      src/graphics/d3d12/backend.cpp)
    target_include_directories(xenon_graphics_d3d12 PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(xenon_graphics_d3d12 PUBLIC xenon_graphics d3d12 dxgi dxguid)
    if(TARGET xenon_graphics_dxc)
      target_link_libraries(xenon_graphics_d3d12 PRIVATE xenon_graphics_dxc)
      target_compile_definitions(xenon_graphics_d3d12 PRIVATE XENON_HAS_DXC=1)
    endif()
    target_compile_features(xenon_graphics_d3d12 PUBLIC cxx_std_20)
    xenon_enable_warnings(xenon_graphics_d3d12)
    add_library(Xenon::GraphicsD3D12 ALIAS xenon_graphics_d3d12)
  endif()
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
    add_executable(xenon_backend_capability_tests
      tests/graphics/backends/backend_capabilities.cpp)
    set_target_properties(xenon_backend_capability_tests PROPERTIES
      OUTPUT_NAME xenon_native_backend_gpu10_tests)
    target_link_libraries(xenon_backend_capability_tests PRIVATE xenon_graphics)
    if(WIN32 AND TARGET xenon_graphics_d3d12)
      target_link_libraries(xenon_backend_capability_tests PRIVATE xenon_graphics_d3d12)
      target_compile_definitions(xenon_backend_capability_tests PRIVATE XENON_TEST_D3D12=1)
    endif()
    if(TARGET xenon_graphics_vulkan)
      target_link_libraries(xenon_backend_capability_tests PRIVATE xenon_graphics_vulkan)
      target_compile_definitions(xenon_backend_capability_tests PRIVATE XENON_TEST_VULKAN=1)
    endif()
    if(TARGET xenon_graphics_dxc AND XENON_DXC_RUNTIME)
      target_link_libraries(xenon_backend_capability_tests PRIVATE xenon_graphics_dxc)
      target_compile_definitions(xenon_backend_capability_tests PRIVATE XENON_TEST_DXC=1)
      add_custom_command(TARGET xenon_backend_capability_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${XENON_DXC_RUNTIME}" "$<TARGET_FILE_DIR:xenon_backend_capability_tests>")
    endif()
    xenon_enable_test_asserts(xenon_backend_capability_tests)
    add_test(NAME xenon_backend_capability_tests COMMAND xenon_backend_capability_tests)

    # Production-backend EDRAM ownership regression. This target is always
    # present so the common fixture is syntax-checked even when no native SDK
    # is installed; available renderer targets opt in below.
    add_executable(xenon_backend_ownership_integration_tests
      tests/graphics/backends/backend_ownership_integration.cpp)
    target_link_libraries(xenon_backend_ownership_integration_tests
      PRIVATE xenon_graphics)
    if(WIN32 AND TARGET xenon_graphics_d3d12)
      target_link_libraries(xenon_backend_ownership_integration_tests
        PRIVATE xenon_graphics_d3d12)
      target_compile_definitions(xenon_backend_ownership_integration_tests
        PRIVATE XENON_TEST_D3D12=1)
    endif()
    if(TARGET xenon_graphics_vulkan)
      target_link_libraries(xenon_backend_ownership_integration_tests
        PRIVATE xenon_graphics_vulkan)
      target_compile_definitions(xenon_backend_ownership_integration_tests
        PRIVATE XENON_TEST_VULKAN=1)
    endif()
    xenon_enable_test_asserts(xenon_backend_ownership_integration_tests)
    add_test(NAME xenon_backend_ownership_integration_tests
      COMMAND xenon_backend_ownership_integration_tests)

    add_executable(xenon_shader_decoder_tests tests/graphics/xenos/shader_decoder.cpp)
    set_target_properties(xenon_shader_decoder_tests PROPERTIES
      OUTPUT_NAME xenon_shader_decoder_gpu09_tests)
    target_link_libraries(xenon_shader_decoder_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_shader_decoder_tests)
    add_test(NAME xenon_shader_decoder_tests COMMAND xenon_shader_decoder_tests)

    add_executable(xenon_depth_format_tests tests/graphics/xenos/depth_format.cpp)
    target_link_libraries(xenon_depth_format_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_depth_format_tests)
    add_test(NAME xenon_depth_format_tests COMMAND xenon_depth_format_tests)

    add_executable(xenon_shader_lowering_tests
      tests/graphics/xenos/shader_lowering.cpp)
    target_link_libraries(xenon_shader_lowering_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_shader_lowering_tests)
    add_test(NAME xenon_shader_lowering_tests COMMAND xenon_shader_lowering_tests)

    add_executable(xenon_resource_ir_tests tests/graphics/xenos/resource_ir.cpp)
    target_link_libraries(xenon_resource_ir_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_resource_ir_tests)
    add_test(NAME xenon_resource_ir_tests COMMAND xenon_resource_ir_tests)

    add_executable(xenon_resource_barrier_tests
      tests/graphics/xenos/resource_barrier.cpp)
    target_link_libraries(xenon_resource_barrier_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_resource_barrier_tests)
    add_test(NAME xenon_resource_barrier_tests COMMAND xenon_resource_barrier_tests)

    add_executable(xenon_texture_tests tests/graphics/xenos/texture.cpp)
    target_link_libraries(xenon_texture_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_texture_tests)
    add_test(NAME xenon_texture_tests COMMAND xenon_texture_tests)

    add_executable(xenon_edram_surface_tests
      tests/graphics/xenos/edram_surface.cpp)
    target_link_libraries(xenon_edram_surface_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_edram_surface_tests)
    add_test(NAME xenon_edram_surface_tests COMMAND xenon_edram_surface_tests)

    add_executable(xenon_edram_ownership_tests
      tests/graphics/xenos/edram_ownership.cpp)
    target_link_libraries(xenon_edram_ownership_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_edram_ownership_tests)
    add_test(NAME xenon_edram_ownership_tests COMMAND xenon_edram_ownership_tests)

    add_executable(xenon_primitive_processor_tests
      tests/graphics/xenos/primitive_processor.cpp)
    target_link_libraries(xenon_primitive_processor_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_primitive_processor_tests)
    add_test(NAME xenon_primitive_processor_tests COMMAND xenon_primitive_processor_tests)

    if(TARGET xenon_graphics_dxc)
      add_executable(xenon_shader_translation_tests
        tests/graphics/xenos/shader_translation.cpp)
      set_target_properties(xenon_shader_translation_tests PROPERTIES
        OUTPUT_NAME xenon_shader_translation_gpu07_tests)
      target_link_libraries(xenon_shader_translation_tests PRIVATE xenon_graphics_dxc)
      xenon_enable_test_asserts(xenon_shader_translation_tests)
      if(XENON_DXC_RUNTIME)
        add_custom_command(TARGET xenon_shader_translation_tests POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${XENON_DXC_RUNTIME}" "$<TARGET_FILE_DIR:xenon_shader_translation_tests>")
      endif()
      add_test(NAME xenon_shader_translation_tests COMMAND xenon_shader_translation_tests)
    endif()

    add_executable(xenon_gpu_frontend_tests tests/graphics/xenos/gpu_frontend.cpp)
    set_target_properties(xenon_gpu_frontend_tests PROPERTIES
      OUTPUT_NAME xenon_gpu_frontend_gpu08_tests)
    target_link_libraries(xenon_gpu_frontend_tests PRIVATE xenon_graphics)
    xenon_enable_test_asserts(xenon_gpu_frontend_tests)
    add_test(NAME xenon_gpu_frontend_tests COMMAND xenon_gpu_frontend_tests)
  endif()
endif()
# -----------------------------------------------------------------------------
# Launcher UI. Kept isolated from the runtime so Qt does not leak into Xenon
# core, CPU, memory, graphics, or game-module interfaces.
if(XENON_BUILD_LAUNCHER)
  add_subdirectory(launcher)
endif()
