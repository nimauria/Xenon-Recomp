# Production install/package rules for Xenon Recomp.
#
# The public packages are self-contained for redistributable Xenon/Qt/audio/
# shader-compiler dependencies. A supported GPU driver remains a host
# requirement because Vulkan ICDs and D3D12 drivers are supplied by the GPU/OS.

include_guard(GLOBAL)
include(GNUInstallDirs)

if(NOT TARGET xenon_runtime_host)
  message(FATAL_ERROR "Xenon packaging requires xenon_runtime_host")
endif()
if(NOT TARGET xenon_launcher)
  message(FATAL_ERROR
    "Xenon production packaging requires the launcher. Configure Qt 6.6+ and XENON_BUILD_LAUNCHER=ON.")
endif()
if(NOT XENON_DEPENDENCY_MODE STREQUAL "MANAGED")
  message(FATAL_ERROR
    "Production packages must use XENON_DEPENDENCY_MODE=MANAGED so releases are reproducible")
endif()
foreach(_required_option IN ITEMS
    XENON_ENABLE_MEMORY XENON_ENABLE_FILESYSTEM XENON_ENABLE_KERNEL
    XENON_ENABLE_GRAPHICS XENON_ENABLE_VULKAN XENON_ENABLE_DXC
    XENON_ENABLE_AUDIO XENON_ENABLE_INPUT)
  if(NOT ${_required_option})
    message(FATAL_ERROR
      "Production packaging requires ${_required_option}=ON")
  endif()
endforeach()
if(NOT TARGET xenon_graphics_vulkan)
  message(FATAL_ERROR "Production packaging requires the Vulkan backend")
endif()
if(WIN32 AND XENON_ENABLE_D3D12 AND NOT TARGET xenon_graphics_d3d12)
  message(FATAL_ERROR "Windows production packaging requires the D3D12 backend")
endif()
if(NOT TARGET xenon_graphics_dxc)
  message(FATAL_ERROR "Production packaging requires the managed DXC shader compiler")
endif()

set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION bin)
if(WIN32)
  # Qt and other MSVC-built redistributables require the VC/UCRT runtime. Copy
  # the legally redistributable runtime DLLs into the installer so users don't
  # need a separate VC++ redistributable installation step.
  set(CMAKE_INSTALL_UCRT_LIBRARIES TRUE)
  include(InstallRequiredSystemLibraries)
endif()

install(FILES
  "${PROJECT_SOURCE_DIR}/LICENSE"
  "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
  "${PROJECT_SOURCE_DIR}/THIRD_PARTY_SOURCE_OFFER.md"
  DESTINATION "share/licenses/Xenon")
if(EXISTS "${PROJECT_SOURCE_DIR}/docs/development/DEPENDENCIES.md")
  install(FILES "${PROJECT_SOURCE_DIR}/docs/development/DEPENDENCIES.md"
    DESTINATION "share/doc/Xenon")
endif()

# Runtime dependencies are installed by runtime_host/CMakeLists.txt so normal
# `cmake --install` builds and CPack use the same payload. Packaging deliberately
# does not add a second copy of those install rules here.


# Keep the open-source Qt license material with every installer. Qt's runtime
# deploy helper copies binaries/plugins but intentionally does not install the
# license directory from the SDK.
if(DEFINED Qt6_DIR AND NOT Qt6_DIR STREQUAL "")
  get_filename_component(XENON_PACKAGING_QT_ROOT "${Qt6_DIR}/../../.." ABSOLUTE)
  if(EXISTS "${XENON_PACKAGING_QT_ROOT}/LICENSES")
    install(DIRECTORY "${XENON_PACKAGING_QT_ROOT}/LICENSES/"
      DESTINATION "share/licenses/Xenon/qt6")
  endif()
endif()

# Qt's QML deploy command handles Windows completely. On Linux it deploys the
# project's QML modules but deliberately doesn't bundle the entire shared Qt
# runtime. Stage the pinned Qt desktop runtime into the install tree so the DEB
# does not require a matching Qt version to already be installed.
if(UNIX AND NOT APPLE)
  find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
  if(NOT DEFINED XENON_PACKAGING_QT_ROOT OR
     NOT EXISTS "${XENON_PACKAGING_QT_ROOT}/lib")
    message(FATAL_ERROR "Unable to locate the Qt installation root for Linux packaging")
  endif()
  configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/StageQtLinux.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/StageQtLinux.cmake"
    @ONLY)
  install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/StageQtLinux.cmake")
endif()

if(UNIX AND NOT APPLE)
  install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/io.xenonrecomp.Xenon.desktop"
    DESTINATION "share/applications")
  install(FILES "${PROJECT_SOURCE_DIR}/launcher/resources/branding/xenon-app-icon-256.png"
    DESTINATION "share/icons/hicolor/256x256/apps"
    RENAME "xenon-recomp.png")
  install(FILES "${PROJECT_SOURCE_DIR}/packaging/linux/io.xenonrecomp.Xenon.metainfo.xml"
    DESTINATION "share/metainfo")
endif()

set(CPACK_PACKAGE_NAME "Xenon Recomp")
set(CPACK_PACKAGE_VENDOR "Xenon Recomp Project")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
  "Native Xbox 360 static-recompilation runtime and launcher")
set(_xenon_package_version "${PROJECT_VERSION}")
if(DEFINED XENON_LAUNCHER_VERSION_OVERRIDE AND
   NOT XENON_LAUNCHER_VERSION_OVERRIDE STREQUAL "")
  set(_xenon_package_version "${XENON_LAUNCHER_VERSION_OVERRIDE}")
endif()
set(CPACK_PACKAGE_VERSION "${_xenon_package_version}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Xenon Recomp")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_STRIP_FILES TRUE)
set(CPACK_VERBATIM_VARIABLES TRUE)

if(WIN32)
  set(CPACK_GENERATOR "NSIS;ZIP")
  set(CPACK_PACKAGE_FILE_NAME "Xenon-Recomp-${_xenon_package_version}-windows-x64")
  set(CPACK_NSIS_DISPLAY_NAME "Xenon Recomp")
  set(CPACK_NSIS_PACKAGE_NAME "Xenon Recomp")
  set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
  set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
  set(CPACK_NSIS_MODIFY_PATH OFF)
  set(CPACK_NSIS_MUI_ICON
    "${PROJECT_SOURCE_DIR}/launcher/resources/windows/xenon-app-icon.ico")
  set(CPACK_NSIS_MUI_UNIICON
    "${PROJECT_SOURCE_DIR}/launcher/resources/windows/xenon-app-icon.ico")
  set(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\xenon_launcher.exe")
  set(CPACK_PACKAGE_EXECUTABLES "xenon_launcher;Xenon Recomp")
  set(CPACK_CREATE_DESKTOP_LINKS xenon_launcher)
elseif(UNIX AND NOT APPLE)
  set(CPACK_GENERATOR "DEB;TGZ")
  set(CPACK_PACKAGE_FILE_NAME "xenon-recomp-${_xenon_package_version}-linux-x64")
  string(REPLACE "-" "~" _xenon_debian_version "${_xenon_package_version}")
  set(CPACK_DEBIAN_PACKAGE_VERSION "${_xenon_debian_version}")
  set(CPACK_DEBIAN_PACKAGE_NAME "xenon-recomp")
  set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Xenon Recomp Project")
  set(CPACK_DEBIAN_PACKAGE_SECTION "games")
  set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
  # GPU vendor drivers/ICDs are deliberately not bundled. CPack computes hard
  # shared-library dependencies from the staged tree; the GPU driver itself is
  # documented as a host requirement rather than guessed as a distro package.
endif()

include(CPack)
