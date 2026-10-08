# -----------------------------------------------------------------------------
# Input: host-neutral Xbox controller model. Platform backends are optional.
add_library(xenon_input_api INTERFACE)
target_include_directories(xenon_input_api INTERFACE "${XENON_SOURCE_DIR}/include")
target_compile_features(xenon_input_api INTERFACE cxx_std_20)
add_library(Xenon::InputAPI ALIAS xenon_input_api)

if(XENON_ENABLE_INPUT)
  add_library(xenon_input STATIC
    action_router.cpp
    system.cpp
    module_api.cpp
    state_merge.cpp
    profile.cpp
    flight_driver.cpp
    xinput/xinput_driver.cpp
    keyboard_mouse_driver.cpp
    xam_facade.cpp
    null_driver.cpp
    sdl/sdl_driver.cpp
  )
  target_include_directories(xenon_input PUBLIC "${XENON_SOURCE_DIR}/include")
  target_link_libraries(xenon_input PUBLIC xenon_input_api)
  target_compile_features(xenon_input PUBLIC cxx_std_20)
  xenon_enable_warnings(xenon_input)
  add_library(Xenon::Input ALIAS xenon_input)

  if(WIN32 AND XENON_INPUT_ENABLE_XINPUT)
    target_sources(xenon_input PRIVATE xinput/xinput_host_win32.cpp)
    target_compile_definitions(xenon_input PUBLIC XENON_INPUT_HAS_XINPUT=1)
  endif()

  if(XENON_INPUT_ENABLE_SDL3)
    find_package(SDL3 3.2 QUIET CONFIG)
    if(TARGET SDL3::SDL3)
      target_sources(xenon_input PRIVATE sdl/sdl3_host.cpp)
      target_link_libraries(xenon_input PRIVATE SDL3::SDL3)
      target_compile_definitions(xenon_input PUBLIC XENON_INPUT_HAS_SDL3=1)
    endif()
  endif()

  if(XENON_INPUT_ENABLE_SDL2)
    xenon_resolve_sdl2(XENON_SDL2_TARGET)
    if(XENON_SDL2_TARGET)
      target_sources(xenon_input PRIVATE sdl/sdl2_host.cpp)
      target_link_libraries(xenon_input PRIVATE ${XENON_SDL2_TARGET})
      target_compile_definitions(xenon_input PUBLIC XENON_INPUT_HAS_SDL2=1)
    endif()
  endif()

  if(XENON_ENABLE_MEMORY)
    add_library(xenon_input_guest STATIC xam_guest.cpp)
    target_include_directories(xenon_input_guest PUBLIC "${XENON_SOURCE_DIR}/include")
    target_link_libraries(xenon_input_guest PUBLIC xenon_input xenon_memory)
    target_compile_features(xenon_input_guest PUBLIC cxx_std_20)
    xenon_enable_warnings(xenon_input_guest)
    add_library(Xenon::InputGuest ALIAS xenon_input_guest)
  endif()
endif()
