# XEX executable format (src/xbox/xex/): parsing, security info and
# decryption, LZX and basic body decompression, PE directories, native import
# tables and title-update deltas. It links no guest kernel or guest memory
# library, so the recompiler and preparation tools can use it on its own.
add_library(xenon_xex STATIC
  xex/xex_loader.cpp
  xex/format/optional_headers.cpp
  xex/security/security_info.cpp
  xex/security/xex_crypto.cpp
  xex/compression/body_decompression.cpp
  xex/compression/xex_lzx.cpp
  xex/image/pe_directories.cpp
  xex/imports/native_imports.cpp
  xex/title_updates/image_delta.cpp
  xex/title_updates/title_update.cpp
)
target_include_directories(xenon_xex
  PUBLIC "${XENON_SOURCE_DIR}/include"
  PRIVATE "${XENON_SOURCE_DIR}/src")
target_compile_features(xenon_xex PUBLIC cxx_std_20)
xenon_enable_warnings(xenon_xex)
add_library(Xenon::Xex ALIAS xenon_xex)

# Maps a parsed XEX image into guest memory (map_xex_image(), load_xex()).
if(XENON_ENABLE_MEMORY)
  add_library(xenon_xex_loading STATIC xex/loading/image_mapping.cpp)
  target_link_libraries(xenon_xex_loading PUBLIC xenon_xex xenon_memory)
  target_include_directories(xenon_xex_loading
    PUBLIC "${XENON_SOURCE_DIR}/include"
    PRIVATE "${XENON_SOURCE_DIR}/src")
  target_compile_features(xenon_xex_loading PUBLIC cxx_std_20)
  xenon_enable_warnings(xenon_xex_loading)
  add_library(Xenon::XexLoading ALIAS xenon_xex_loading)
endif()

# Guest xboxkrnl ABI bridge: exports, import and module registries and guest
# I/O. Kept separate so Xenon::Kernel can still be built/tested without guest
# memory.
if(XENON_ENABLE_KERNEL AND XENON_ENABLE_MEMORY)
  add_library(xenon_xbox_kernel_io STATIC
    guest_io/xbox_io_guest.cpp
    imports/import_registry.cpp
    exports/xboxkrnl_io_exports.cpp
    exports/xboxkrnl_memory_exports.cpp
    exports/xboxkrnl_process_exports.cpp
    exports/xboxkrnl_rtl_critical_section_exports.cpp
    exports/xboxkrnl_xex_module_exports.cpp
    exports/xboxkrnl_tls_exports.cpp
    exports/xboxkrnl_video_exports.cpp
    exports/xboxkrnl_device_io_exports.cpp
    exports/xboxkrnl_rtl_exports.cpp
    exports/xboxkrnl_time_exports.cpp
    exports/xboxkrnl_sync_exports.cpp
    exports/xboxkrnl_ke_sync_exports.cpp
    exports/xboxkrnl_ke_irql_exports.cpp
    exports/xboxkrnl_misc_exports.cpp
    exports/xboxkrnl_ob_exports.cpp
    exports/xboxkrnl_string_exports.cpp
    exports/xboxkrnl_rtl_string_exports.cpp
    exports/xboxkrnl_threading_exports.cpp
    exports/xboxkrnl_pool_exports.cpp
    module_registry.cpp
    string_format.cpp
    xbox_time_convert.cpp
    xex_dispatcher_header.cpp
    rtl.cpp
    export_metadata.cpp
  )
  target_link_libraries(xenon_xbox_kernel_io
    PUBLIC xenon_kernel xenon_memory xenon_xex xenon_xex_loading)
  target_include_directories(xenon_xbox_kernel_io
    PUBLIC "${XENON_SOURCE_DIR}/include"
    PRIVATE "${XENON_SOURCE_DIR}/src")
  target_compile_features(xenon_xbox_kernel_io PUBLIC cxx_std_20)
  xenon_enable_warnings(xenon_xbox_kernel_io)
  add_library(Xenon::XboxKernelIo ALIAS xenon_xbox_kernel_io)
endif()
