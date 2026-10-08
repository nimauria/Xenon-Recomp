# -----------------------------------------------------------------------------
# Xenon Network client/uplink. The protocol and service boundary are kept in a
# Qt-free, platform-neutral target. Concrete HTTPS/realtime transports live at
# the host edge (the launcher supplies the first Qt implementation) so Xbox/XAM
# compatibility code never sees URLs, JSON, or a particular socket library.
if(XENON_ENABLE_NETWORK)
  add_library(xenon_network STATIC
    types.cpp
    routes.cpp
    client/client.cpp
    client/bootstrap.cpp
    client/endpoint.cpp
    client/protocol.cpp
    client/client_internal.hpp
    realtime.cpp
    xbox_services.cpp
  )
  target_include_directories(xenon_network
    PUBLIC "${XENON_SOURCE_DIR}/include"
    PRIVATE "${XENON_SOURCE_DIR}/src")
  target_compile_features(xenon_network PUBLIC cxx_std_20)
  target_link_libraries(xenon_network PUBLIC Xenon::Core)
  xenon_enable_warnings(xenon_network)
  add_library(Xenon::Network ALIAS xenon_network)
endif()
