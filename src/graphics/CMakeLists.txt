# -----------------------------------------------------------------------------
# Graphics: Xenos PM4/shader/EDRAM frontend. Vulkan and D3D12 remain backends.
if(XENON_ENABLE_GRAPHICS)
  add_library(xenon_graphics STATIC
    xenos/register_file.cpp
    xenos/register_aperture.cpp
    xenos/shader.cpp
    xenos/shader_decoder.cpp
    xenos/shader_translation.cpp
    xenos/depth_format.cpp
    xenos/resource_ir.cpp
    xenos/resource_barrier.cpp
    xenos/texture.cpp
    xenos/presentation.cpp
    xenos/edram.cpp
    xenos/edram_surface.cpp
    xenos/edram_ownership.cpp
    xenos/primitive_processor.cpp
    xenos/command_processor.cpp
    xenos/capture.cpp
    xenos/graphics_system.cpp
    common/backend_capabilities.cpp
  )
  target_link_libraries(xenon_graphics PUBLIC xenon_memory xenon_logging)
  if(CMAKE_DL_LIBS)
    target_link_libraries(xenon_graphics PRIVATE ${CMAKE_DL_LIBS})
  endif()
  if(WIN32)
    # Runtime capability discovery lives in the common layer and calls DXGI
    # and D3D12 even when the optional renderer target is disabled.
    target_link_libraries(xenon_graphics PRIVATE d3d12 dxgi)
  endif()
  target_include_directories(xenon_graphics PUBLIC "${XENON_SOURCE_DIR}/include")
  target_compile_features(xenon_graphics PUBLIC cxx_std_20)
  xenon_enable_warnings(xenon_graphics)
  add_library(Xenon::Graphics ALIAS xenon_graphics)
  add_library(Xenon::GPU ALIAS xenon_graphics)
  # Compatibility with the phase-1 working tree target name.
  add_library(xenon_gpu ALIAS xenon_graphics)

  if(XENON_ENABLE_DXC)
    xenon_resolve_dxc(XENON_DXC_TARGET XENON_DXC_INCLUDE_DIR)
    if(XENON_DXC_TARGET AND XENON_DXC_INCLUDE_DIR)
      add_library(xenon_graphics_dxc STATIC
        dxc/shader_compiler.cpp)
      target_include_directories(xenon_graphics_dxc
        PUBLIC "${XENON_SOURCE_DIR}/include"
        PRIVATE "${XENON_DXC_INCLUDE_DIR}")
      target_link_libraries(xenon_graphics_dxc PUBLIC xenon_graphics
        PRIVATE ${XENON_DXC_TARGET})
      target_compile_features(xenon_graphics_dxc PUBLIC cxx_std_20)
      xenon_enable_warnings(xenon_graphics_dxc)
      add_library(Xenon::GraphicsDXC ALIAS xenon_graphics_dxc)
      message(STATUS "DXC shader compiler enabled through Xenon dependency resolver")
    else()
      message(STATUS "DXC shader compiler disabled: compatible DXC runtime/development files not found")
    endif()
  endif()

  if(XENON_ENABLE_VULKAN)
    xenon_resolve_vulkan(XENON_VULKAN_TARGET)
    if(XENON_VULKAN_TARGET)
      target_compile_definitions(xenon_graphics PRIVATE XENON_HAS_VULKAN_SDK=1)
      add_library(xenon_graphics_vulkan STATIC
        vulkan/context.cpp
        vulkan/command_queue.cpp
        vulkan/memory.cpp
        vulkan/guest_memory_mirror.cpp
        vulkan/resource_layout.cpp
        vulkan/pipeline.cpp
        vulkan/render_target.cpp
        vulkan/depth_target.cpp
        vulkan/texture.cpp
        vulkan/presentation.cpp
        vulkan/backend.cpp
        common/backend_core.hpp
        common/backend_core_consume.hpp
        common/backend_core_draw.hpp
        common/backend_core_submission.hpp)
      target_include_directories(xenon_graphics_vulkan
        PUBLIC "${XENON_SOURCE_DIR}/include"
        PRIVATE "${XENON_SOURCE_DIR}/src")
      target_link_libraries(xenon_graphics_vulkan PUBLIC xenon_graphics ${XENON_VULKAN_TARGET})
      if(TARGET xenon_graphics_dxc)
        target_link_libraries(xenon_graphics_vulkan PRIVATE xenon_graphics_dxc)
        target_compile_definitions(xenon_graphics_vulkan PRIVATE XENON_HAS_DXC=1)
      endif()
      target_compile_features(xenon_graphics_vulkan PUBLIC cxx_std_20)
      xenon_enable_warnings(xenon_graphics_vulkan)
      add_library(Xenon::GraphicsVulkan ALIAS xenon_graphics_vulkan)
    else()
      message(STATUS "Vulkan runtime backend disabled: no compatible Vulkan loader/headers were resolved")
    endif()
  endif()

  if(WIN32 AND XENON_ENABLE_D3D12)
    add_library(xenon_graphics_d3d12 STATIC
      d3d12/context.cpp
      d3d12/command_queue.cpp
      d3d12/memory.cpp
      d3d12/guest_memory_mirror.cpp
      d3d12/resource_layout.cpp
      d3d12/pipeline.cpp
      d3d12/render_target.cpp
      d3d12/depth_target.cpp
      d3d12/texture.cpp
      d3d12/presentation.cpp
      d3d12/backend.cpp
      common/backend_core.hpp
      common/backend_core_consume.hpp
      common/backend_core_draw.hpp
      common/backend_core_submission.hpp)
    target_include_directories(xenon_graphics_d3d12
      PUBLIC "${XENON_SOURCE_DIR}/include"
      PRIVATE "${XENON_SOURCE_DIR}/src")
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
