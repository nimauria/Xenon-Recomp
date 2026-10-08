# -----------------------------------------------------------------------------
# Filesystem: host-independent Xbox-style VFS and content providers.
# This target deliberately remains independent of guest Memory.
if(XENON_ENABLE_FILESYSTEM)
  add_library(xenon_filesystem STATIC
    content/xex_metadata.cpp
    content/content_probe.cpp
    content/gdfx_image_source.cpp
    content/stfs_package.cpp
    content/stfs_package_source.cpp
    content/content_materializer.cpp
    virtual/path.cpp
    virtual/device.cpp
    virtual/directory_cursor.cpp
    virtual/virtual_file_system.cpp
    devices/null_device.cpp
    devices/read_only_content_device.cpp
    host/host_path_device.cpp
  )
  target_include_directories(xenon_filesystem PUBLIC "${XENON_SOURCE_DIR}/include")
  target_compile_features(xenon_filesystem PUBLIC cxx_std_20)
  xenon_enable_warnings(xenon_filesystem)
  add_library(Xenon::Filesystem ALIAS xenon_filesystem)
endif()
