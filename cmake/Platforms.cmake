if(WIN32)
  set(XENON_HOST_PLATFORM "windows")
elseif(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  # Keep the public host-platform name aligned with the managed dependency
  # triplet (macos-arm64) instead of leaking CMake's kernel name (Darwin).
  set(XENON_HOST_PLATFORM "macos")
elseif(ANDROID)
  set(XENON_HOST_PLATFORM "android")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(XENON_HOST_PLATFORM "linux")
else()
  string(TOLOWER "${CMAKE_SYSTEM_NAME}" XENON_HOST_PLATFORM)
endif()
