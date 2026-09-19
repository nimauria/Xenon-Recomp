find_package(Qt6 6.6 QUIET COMPONENTS Core Gui Network Quick QuickControls2 QuickDialogs2)

if(NOT Qt6_FOUND)
  message(STATUS
    "Xenon launcher disabled: Qt 6.6+ Core/Gui/Network/Quick/QuickControls2/QuickDialogs2 was not found")
  return()
endif()

qt_standard_project_setup(REQUIRES 6.6)
if(QT_KNOWN_POLICY_QTP0004)
  qt_policy(SET QTP0004 NEW)
endif()

option(XENON_LAUNCHER_TEST_MODE
  "Populate the Xenon launcher with fictional front-end test data" OFF)
option(XENON_LAUNCHER_ENABLE_DISCORD_RICH_PRESENCE
  "Build the optional Discord Rich Presence provider" OFF)
set(XENON_LAUNCHER_VERSION_OVERRIDE "" CACHE STRING
  "Override the launcher version embedded in release builds")
set(XENON_LAUNCHER_UPDATE_REPOSITORY "nimauria/Xenon-Recomp" CACHE STRING
  "GitHub owner/repository used by the launcher update provider")
set(XENON_LAUNCHER_DEFAULT_DISCORD_APPLICATION_ID "1550625950570520768")
set(XENON_LAUNCHER_DISCORD_APPLICATION_ID "${XENON_LAUNCHER_DEFAULT_DISCORD_APPLICATION_ID}" CACHE STRING
  "Discord application ID used by the launcher Rich Presence provider")
# Builds configured before Xenon had a public Discord application may have an
# empty value cached already. Migrate that legacy empty cache entry to the
# official Xenon application ID. Forks remain free to override this with any
# other non-empty application ID through the normal CMake cache variable.
if(XENON_LAUNCHER_DISCORD_APPLICATION_ID STREQUAL "")
  set(XENON_LAUNCHER_DISCORD_APPLICATION_ID
      "${XENON_LAUNCHER_DEFAULT_DISCORD_APPLICATION_ID}" CACHE STRING
      "Discord application ID used by the launcher Rich Presence provider" FORCE)
endif()
set(XENON_DISCORD_SOCIAL_SDK_ROOT "" CACHE PATH
  "Root of the Discord Social SDK C++ package (contains include/, lib/ and bin/)")
set(XENON_DISCORD_SOCIAL_SDK_INCLUDE_DIR "" CACHE PATH
  "Directory containing Discord Social SDK discordpp.h (normally inferred from XENON_DISCORD_SOCIAL_SDK_ROOT)")
set(XENON_DISCORD_SOCIAL_SDK_LIBRARY "" CACHE FILEPATH
  "Discord Social SDK import/shared library (normally inferred from XENON_DISCORD_SOCIAL_SDK_ROOT)")
set(XENON_DISCORD_SOCIAL_SDK_RUNTIME "" CACHE FILEPATH
  "Discord Social SDK runtime shared library to deploy beside the launcher executable")
option(XENON_LAUNCHER_REQUIRE_DISCORD_SOCIAL_SDK
  "Fail launcher configuration when the Discord Social SDK cannot be found" OFF)
if(XENON_LAUNCHER_REQUIRE_DISCORD_SOCIAL_SDK AND NOT XENON_LAUNCHER_ENABLE_DISCORD_RICH_PRESENCE)
  set(XENON_LAUNCHER_ENABLE_DISCORD_RICH_PRESENCE ON CACHE BOOL
      "Build the optional Discord Rich Presence provider" FORCE)
endif()

# Rich Presence is intentionally dormant by default. Community/support links remain
# available without the Discord SDK. Enable this option explicitly when Xenon is
# ready to ship Rich Presence.
if(XENON_LAUNCHER_ENABLE_DISCORD_RICH_PRESENCE)
  # Prefer an explicit cache path, then the environment, then Xenon's conventional
  # local vendor directory. The SDK itself is intentionally not committed: Discord
  # distributes it through the Developer Portal for the application owner.
  if(NOT XENON_DISCORD_SOCIAL_SDK_ROOT AND DEFINED ENV{DISCORD_SOCIAL_SDK_ROOT})
    file(TO_CMAKE_PATH "$ENV{DISCORD_SOCIAL_SDK_ROOT}" _xenon_discord_env_root)
    set(XENON_DISCORD_SOCIAL_SDK_ROOT "${_xenon_discord_env_root}" CACHE PATH
        "Root of the Discord Social SDK C++ package (contains include/, lib/ and bin/)" FORCE)
  endif()
  if(NOT XENON_DISCORD_SOCIAL_SDK_ROOT)
    foreach(_xenon_discord_candidate IN ITEMS
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/discord_social_sdk"
        "${CMAKE_SOURCE_DIR}/third_party/discord_social_sdk")
      if(EXISTS "${_xenon_discord_candidate}/include/discordpp.h")
        set(XENON_DISCORD_SOCIAL_SDK_ROOT "${_xenon_discord_candidate}" CACHE PATH
            "Root of the Discord Social SDK C++ package (contains include/, lib/ and bin/)" FORCE)
        break()
      endif()
    endforeach()
  endif()

  if(XENON_DISCORD_SOCIAL_SDK_ROOT)
    if(NOT XENON_DISCORD_SOCIAL_SDK_INCLUDE_DIR)
      set(XENON_DISCORD_SOCIAL_SDK_INCLUDE_DIR
          "${XENON_DISCORD_SOCIAL_SDK_ROOT}/include" CACHE PATH
          "Directory containing Discord Social SDK discordpp.h (normally inferred from XENON_DISCORD_SOCIAL_SDK_ROOT)" FORCE)
    endif()

    # Discord's standalone C++ archive uses debug/release subdirectories. Xenon's
    # normal Ninja build is single-config, so select the matching package here.
    string(TOLOWER "${CMAKE_BUILD_TYPE}" _xenon_discord_build_type)
    if(_xenon_discord_build_type STREQUAL "debug")
      set(_xenon_discord_config_dir "debug")
    else()
      set(_xenon_discord_config_dir "release")
    endif()

    if(WIN32)
      if(NOT XENON_DISCORD_SOCIAL_SDK_LIBRARY)
        set(XENON_DISCORD_SOCIAL_SDK_LIBRARY
            "${XENON_DISCORD_SOCIAL_SDK_ROOT}/lib/${_xenon_discord_config_dir}/discord_partner_sdk.lib"
            CACHE FILEPATH "Discord Social SDK import/shared library" FORCE)
      endif()
      if(NOT XENON_DISCORD_SOCIAL_SDK_RUNTIME)
        set(XENON_DISCORD_SOCIAL_SDK_RUNTIME
            "${XENON_DISCORD_SOCIAL_SDK_ROOT}/bin/${_xenon_discord_config_dir}/discord_partner_sdk.dll"
            CACHE FILEPATH "Discord Social SDK runtime shared library to deploy beside the launcher executable" FORCE)
      endif()
    elseif(APPLE)
      # Kept overrideable because recent Social SDK drops may package macOS as a
      # framework instead of the legacy dylib layout.
      if(NOT XENON_DISCORD_SOCIAL_SDK_LIBRARY)
        set(XENON_DISCORD_SOCIAL_SDK_LIBRARY
            "${XENON_DISCORD_SOCIAL_SDK_ROOT}/lib/${_xenon_discord_config_dir}/libdiscord_partner_sdk.dylib"
            CACHE FILEPATH "Discord Social SDK import/shared library" FORCE)
      endif()
      if(NOT XENON_DISCORD_SOCIAL_SDK_RUNTIME)
        set(XENON_DISCORD_SOCIAL_SDK_RUNTIME "${XENON_DISCORD_SOCIAL_SDK_LIBRARY}" CACHE FILEPATH
            "Discord Social SDK runtime shared library to deploy beside the launcher executable" FORCE)
      endif()
    else()
      if(NOT XENON_DISCORD_SOCIAL_SDK_LIBRARY)
        set(XENON_DISCORD_SOCIAL_SDK_LIBRARY
            "${XENON_DISCORD_SOCIAL_SDK_ROOT}/lib/${_xenon_discord_config_dir}/libdiscord_partner_sdk.so"
            CACHE FILEPATH "Discord Social SDK import/shared library" FORCE)
      endif()
      if(NOT XENON_DISCORD_SOCIAL_SDK_RUNTIME)
        set(XENON_DISCORD_SOCIAL_SDK_RUNTIME "${XENON_DISCORD_SOCIAL_SDK_LIBRARY}" CACHE FILEPATH
            "Discord Social SDK runtime shared library to deploy beside the launcher executable" FORCE)
      endif()
    endif()
  endif()
endif()
if(XENON_LAUNCHER_VERSION_OVERRIDE)
  set(XENON_LAUNCHER_VERSION_VALUE "${XENON_LAUNCHER_VERSION_OVERRIDE}")
else()
  set(XENON_LAUNCHER_VERSION_VALUE "${PROJECT_VERSION}-dev")
endif()

qt_add_library(xenon_launcher_core STATIC
  src/models/launch_configuration.cpp
  src/models/launch_configuration.hpp
  src/models/session_record.cpp
  src/models/session_record.hpp
  src/core/launcher_core.cpp
  src/core/launcher_core.hpp
  src/runtime/content_probe.cpp
  src/runtime/content_probe.hpp
  src/runtime/runtime_bridge.cpp
  src/runtime/runtime_bridge.hpp
  src/services/service_result.hpp
  src/services/content_import_service.cpp
  src/services/content_import_service.hpp
  src/services/settings_service.cpp
  src/services/settings_service.hpp
  src/services/path_service.cpp
  src/services/path_service.hpp
  src/services/profile_service.cpp
  src/services/profile_service.hpp
  src/services/recovery_service.cpp
  src/services/recovery_service.hpp
  src/services/package_service.cpp
  src/services/package_service.hpp
  src/services/library_service.cpp
  src/services/library_service.hpp
  src/services/dlc_service.cpp
  src/services/dlc_service.hpp
  src/services/module_service.cpp
  src/services/module_service.hpp
  src/services/launch_service.cpp
  src/services/launch_service.hpp
  src/frontend_backend/frontend_backend.cpp
  src/frontend_backend/frontend_backend.hpp
  src/frontend_backend/application/application_feature.cpp
  src/frontend_backend/application/application_feature.hpp
  src/frontend_backend/application/recovery/recovery_feature.cpp
  src/frontend_backend/application/recovery/recovery_feature.hpp
  src/frontend_backend/branding/branding_feature.cpp
  src/frontend_backend/branding/branding_feature.hpp
  src/frontend_backend/community/community_feature.cpp
  src/frontend_backend/community/community_feature.hpp
  src/frontend_backend/community/discord/discord_presence_feature.cpp
  src/frontend_backend/community/discord/discord_presence_feature.hpp
  src/frontend_backend/community/discord/discord_presence_provider.cpp
  src/frontend_backend/community/discord/discord_presence_provider.hpp
  src/frontend_backend/diagnostics/diagnostics_feature.cpp
  src/frontend_backend/diagnostics/diagnostics_feature.hpp
  src/frontend_backend/diagnostics/support/support_bundle_service.cpp
  src/frontend_backend/diagnostics/support/support_bundle_service.hpp
  src/frontend_backend/filesystem/filesystem_feature.cpp
  src/frontend_backend/filesystem/filesystem_feature.hpp
  src/frontend_backend/home/home_feature.cpp
  src/frontend_backend/home/home_feature.hpp
  src/frontend_backend/import_export/import_export_feature.cpp
  src/frontend_backend/import_export/import_export_feature.hpp
  src/frontend_backend/input/input_feature.cpp
  src/frontend_backend/input/input_feature.hpp
  src/frontend_backend/launch/launch_feature.cpp
  src/frontend_backend/launch/launch_feature.hpp
  src/frontend_backend/launch/session/session_controller.cpp
  src/frontend_backend/launch/session/session_controller.hpp
  src/frontend_backend/library/library_feature.cpp
  src/frontend_backend/library/library_feature.hpp
  src/frontend_backend/library/actions/library_action_catalog.cpp
  src/frontend_backend/library/actions/library_action_catalog.hpp
  src/frontend_backend/library/dlc/dlc_feature.cpp
  src/frontend_backend/library/dlc/dlc_feature.hpp
  src/frontend_backend/library/dlc/actions/dlc_action_catalog.cpp
  src/frontend_backend/library/dlc/actions/dlc_action_catalog.hpp
  src/frontend_backend/library/properties/game_properties_feature.cpp
  src/frontend_backend/library/properties/game_properties_feature.hpp
  src/frontend_backend/modules/modules_feature.cpp
  src/frontend_backend/modules/modules_feature.hpp
  src/frontend_backend/modules/actions/module_action_catalog.cpp
  src/frontend_backend/modules/actions/module_action_catalog.hpp
  src/frontend_backend/modules/catalog/github_module_catalog_provider.cpp
  src/frontend_backend/modules/catalog/github_module_catalog_provider.hpp
  src/frontend_backend/modules/catalog/assets/module_catalog_asset_cache.cpp
  src/frontend_backend/modules/catalog/assets/module_catalog_asset_cache.hpp
  src/frontend_backend/modules/import/module_import_service.cpp
  src/frontend_backend/modules/import/module_import_service.hpp
  src/frontend_backend/modules/updates/module_update_service.cpp
  src/frontend_backend/modules/updates/module_update_service.hpp
  src/frontend_backend/modules/updates/history/module_update_history_store.cpp
  src/frontend_backend/modules/updates/history/module_update_history_store.hpp
  src/frontend_backend/modules/package/module_package_installer.cpp
  src/frontend_backend/modules/package/module_package_installer.hpp
  src/frontend_backend/notifications/notification_center_feature.cpp
  src/frontend_backend/notifications/notification_center_feature.hpp
  src/frontend_backend/paths/paths_feature.cpp
  src/frontend_backend/paths/paths_feature.hpp
  src/frontend_backend/profiles/profiles_feature.cpp
  src/frontend_backend/profiles/profiles_feature.hpp
  src/frontend_backend/profiles/actions/profile_action_catalog.cpp
  src/frontend_backend/profiles/actions/profile_action_catalog.hpp
  src/frontend_backend/profiles/runtime/profile_runtime_catalog.cpp
  src/frontend_backend/profiles/runtime/profile_runtime_catalog.hpp
  src/frontend_backend/runtime/runtime_feature.cpp
  src/frontend_backend/runtime/runtime_feature.hpp
  src/frontend_backend/search/command_palette_feature.cpp
  src/frontend_backend/search/command_palette_feature.hpp
  src/frontend_backend/settings/settings_feature.cpp
  src/frontend_backend/settings/settings_feature.hpp
  src/frontend_backend/settings/schema/settings_catalog.cpp
  src/frontend_backend/settings/schema/settings_catalog.hpp
  src/frontend_backend/settings/appearance/appearance_feature.cpp
  src/frontend_backend/settings/appearance/appearance_feature.hpp
  src/frontend_backend/updates/update_feature.cpp
  src/frontend_backend/updates/update_feature.hpp
  src/frontend_backend/updates/model/update_release.cpp
  src/frontend_backend/updates/model/update_release.hpp
  src/frontend_backend/updates/providers/update_provider.hpp
  src/frontend_backend/updates/providers/github_release_provider.cpp
  src/frontend_backend/updates/providers/github_release_provider.hpp
  src/frontend_backend/updates/install/update_installer.cpp
  src/frontend_backend/updates/install/update_installer.hpp
  src/frontend_backend/updates/version/semantic_version.cpp
  src/frontend_backend/updates/version/semantic_version.hpp
)
target_include_directories(xenon_launcher_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(xenon_launcher_core PUBLIC Qt6::Core Qt6::Gui Qt6::Network Xenon::Core)
if(TARGET xenon_input)
  target_link_libraries(xenon_launcher_core PRIVATE Xenon::Input)
endif()
target_compile_features(xenon_launcher_core PUBLIC cxx_std_20)

set(XENON_LAUNCHER_HAS_DISCORD_SOCIAL_SDK OFF)
set(_xenon_discord_missing "")
if(XENON_LAUNCHER_ENABLE_DISCORD_RICH_PRESENCE)
  if(NOT XENON_LAUNCHER_DISCORD_APPLICATION_ID)
    list(APPEND _xenon_discord_missing "application ID")
  endif()
  if(NOT EXISTS "${XENON_DISCORD_SOCIAL_SDK_INCLUDE_DIR}/discordpp.h")
    list(APPEND _xenon_discord_missing "discordpp.h")
  endif()
  if(NOT EXISTS "${XENON_DISCORD_SOCIAL_SDK_LIBRARY}")
    list(APPEND _xenon_discord_missing "link library")
  endif()
  if(WIN32 AND NOT EXISTS "${XENON_DISCORD_SOCIAL_SDK_RUNTIME}")
    list(APPEND _xenon_discord_missing "discord_partner_sdk.dll")
  endif()

  if(NOT _xenon_discord_missing)
    set(XENON_LAUNCHER_HAS_DISCORD_SOCIAL_SDK ON)
    target_include_directories(xenon_launcher_core PRIVATE "${XENON_DISCORD_SOCIAL_SDK_INCLUDE_DIR}")
    target_link_libraries(xenon_launcher_core PRIVATE "${XENON_DISCORD_SOCIAL_SDK_LIBRARY}")
  endif()

  if(XENON_LAUNCHER_HAS_DISCORD_SOCIAL_SDK)
    message(STATUS "Xenon launcher Discord Rich Presence: provider enabled")
    message(STATUS "  Discord SDK root: ${XENON_DISCORD_SOCIAL_SDK_ROOT}")
    message(STATUS "  Discord SDK library: ${XENON_DISCORD_SOCIAL_SDK_LIBRARY}")
    message(STATUS "  Discord SDK runtime: ${XENON_DISCORD_SOCIAL_SDK_RUNTIME}")
  else()
    string(JOIN ", " _xenon_discord_missing_text ${_xenon_discord_missing})
    if(XENON_LAUNCHER_REQUIRE_DISCORD_SOCIAL_SDK)
      message(FATAL_ERROR
        "Xenon launcher Discord Rich Presence was required but the Social SDK is incomplete: ${_xenon_discord_missing_text}. "
        "Download the C++ SDK from the Discord Developer Portal and set XENON_DISCORD_SOCIAL_SDK_ROOT.")
    endif()
    message(STATUS
      "Xenon launcher Discord Rich Presence: requested but unavailable (${_xenon_discord_missing_text})")
  endif()
else()
  message(STATUS "Xenon launcher Discord Rich Presence: disabled (community links remain available)")
endif()

set(XENON_LAUNCHER_HAS_MEMORY OFF)
set(XENON_LAUNCHER_HAS_GRAPHICS OFF)
set(XENON_LAUNCHER_HAS_AUDIO OFF)
set(XENON_LAUNCHER_HAS_INPUT OFF)
set(XENON_LAUNCHER_HAS_NETWORK OFF)
set(XENON_LAUNCHER_HAS_VULKAN OFF)
set(XENON_LAUNCHER_HAS_D3D12 OFF)
if(TARGET xenon_memory)
  set(XENON_LAUNCHER_HAS_MEMORY ON)
endif()
if(TARGET xenon_graphics)
  set(XENON_LAUNCHER_HAS_GRAPHICS ON)
endif()
if(TARGET xenon_graphics_vulkan)
  set(XENON_LAUNCHER_HAS_VULKAN ON)
endif()
if(TARGET xenon_graphics_d3d12)
  set(XENON_LAUNCHER_HAS_D3D12 ON)
endif()
if(TARGET xenon_audio)
  set(XENON_LAUNCHER_HAS_AUDIO ON)
endif()
if(TARGET xenon_input)
  set(XENON_LAUNCHER_HAS_INPUT ON)
endif()
if(TARGET xenon_network)
  set(XENON_LAUNCHER_HAS_NETWORK ON)
endif()
target_compile_definitions(xenon_launcher_core PRIVATE
  XENON_LAUNCHER_VERSION="${XENON_LAUNCHER_VERSION_VALUE}"
  XENON_LAUNCHER_UPDATE_REPOSITORY="${XENON_LAUNCHER_UPDATE_REPOSITORY}"
  XENON_LAUNCHER_DISCORD_APPLICATION_ID="${XENON_LAUNCHER_DISCORD_APPLICATION_ID}"
  XENON_LAUNCHER_DISCORD_RICH_PRESENCE_ENABLED=$<BOOL:${XENON_LAUNCHER_ENABLE_DISCORD_RICH_PRESENCE}>
  XENON_LAUNCHER_HAS_DISCORD_SOCIAL_SDK=$<BOOL:${XENON_LAUNCHER_HAS_DISCORD_SOCIAL_SDK}>
  XENON_LAUNCHER_TEST_MODE=$<BOOL:${XENON_LAUNCHER_TEST_MODE}>
  XENON_LAUNCHER_RUNTIME_MEMORY=$<BOOL:${XENON_LAUNCHER_HAS_MEMORY}>
  XENON_LAUNCHER_RUNTIME_GRAPHICS=$<BOOL:${XENON_LAUNCHER_HAS_GRAPHICS}>
  XENON_LAUNCHER_RUNTIME_AUDIO=$<BOOL:${XENON_LAUNCHER_HAS_AUDIO}>
  XENON_LAUNCHER_RUNTIME_INPUT=$<BOOL:${XENON_LAUNCHER_HAS_INPUT}>
  XENON_LAUNCHER_RUNTIME_NETWORK=$<BOOL:${XENON_LAUNCHER_HAS_NETWORK}>
  XENON_LAUNCHER_RUNTIME_VULKAN=$<BOOL:${XENON_LAUNCHER_HAS_VULKAN}>
  XENON_LAUNCHER_RUNTIME_D3D12=$<BOOL:${XENON_LAUNCHER_HAS_D3D12}>
)
xenon_enable_warnings(xenon_launcher_core)
add_library(Xenon::LauncherCore ALIAS xenon_launcher_core)

qt_add_executable(xenon_launcher
  src/main.cpp
  src/launcher_bridge.cpp
  src/launcher_bridge.hpp
  src/launcher_config.hpp
)

if(XENON_LAUNCHER_HAS_DISCORD_SOCIAL_SDK AND EXISTS "${XENON_DISCORD_SOCIAL_SDK_RUNTIME}")
  add_custom_command(TARGET xenon_launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${XENON_DISCORD_SOCIAL_SDK_RUNTIME}" "$<TARGET_FILE_DIR:xenon_launcher>")
endif()

set_source_files_properties(
  qml/Theme.qml
  qml/ProfileStore.qml
  PROPERTIES QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(xenon_launcher
  URI Xenon.Launcher
  VERSION 1.0
  QML_FILES
    qml/Main.qml
    qml/Theme.qml
    qml/ProfileStore.qml
    qml/ProfileMenu.qml
    qml/TopBar.qml
    qml/CommandPalette.qml
    qml/NotificationCenter.qml
    qml/HomePage.qml
    qml/Sidebar.qml
    qml/FooterBar.qml
    qml/XPanel.qml
    qml/XButton.qml
    qml/XIconButton.qml
    qml/XWindowButton.qml
    qml/XenonBrand.qml
    qml/ProfileAvatar.qml
    qml/ModuleSettingsDialog.qml
    qml/GamePropertiesDialog.qml
    qml/ModuleCatalogDialog.qml
    qml/HelpPopover.qml
    qml/RecoveryDialog.qml
    qml/XNavButton.qml
    qml/XIcon.qml
    qml/XTextField.qml
    qml/XComboBox.qml
    qml/XSlider.qml
    qml/XSettingsCard.qml
    qml/XSettingsPage.qml
    qml/XSectionHeader.qml
    qml/XInfoRow.qml
    qml/XActionMenu.qml
    qml/XSwitch.qml
    qml/XPathField.qml
    qml/StatusPill.qml
    qml/ArtworkFrame.qml
    qml/GameTile.qml
    qml/Toast.qml
    qml/EmptyState.qml
    qml/ProfileEditorDialog.qml
    qml/XConfirmDialog.qml
    qml/ThemeBackdrop.qml
    qml/LibraryPage.qml
    qml/ModulesPage.qml
    qml/ProfilesPage.qml
    qml/SettingsPage.qml
)


qt_add_resources(xenon_launcher xenon_branding
  PREFIX "/branding"
  BASE "resources/branding"
  FILES
    resources/branding/xenon-mark.svg
    resources/branding/xenon-icon.svg
    resources/branding/xenon-wordmark.svg
    resources/branding/xenon-lockup.svg
    resources/branding/xenon-lockup-full.svg
    resources/branding/xenon-profile-mark.svg
    resources/branding/xenon-app-icon-256.png
)

qt_add_resources(xenon_launcher xenon_theme_art
  PREFIX "/theme-art"
  BASE "resources"
  FILES
    resources/backgrounds/theme-xenon-dark-orbit.png
    resources/backgrounds/theme-xenon-dark-tech.png
    resources/backgrounds/theme-xenon-dark-hud.png
    resources/backgrounds/theme-carbon-nebula.png
    resources/backgrounds/theme-carbon-tech.png
    resources/backgrounds/theme-industrial-orbit.png
    resources/backgrounds/theme-industrial-tech.png
    resources/decor/amber/badge_tag_amber.svg
    resources/decor/amber/corner_bottom_right_amber.svg
    resources/decor/amber/corner_top_left_amber.svg
    resources/decor/amber/divider_long_amber.svg
    resources/decor/amber/divider_notch_amber.svg
    resources/decor/amber/hud_arc_left_amber.svg
    resources/decor/amber/loading_bar_amber.svg
    resources/decor/amber/panel_frame_amber.svg
    resources/decor/amber/rail_vertical_amber.svg
    resources/decor/amber/side_strip_amber.svg
    resources/decor/amber/slashes_amber.svg
    resources/decor/blue/badge_tag_blue.svg
    resources/decor/blue/corner_bottom_right_blue.svg
    resources/decor/blue/corner_top_left_blue.svg
    resources/decor/blue/divider_long_blue.svg
    resources/decor/blue/divider_notch_blue.svg
    resources/decor/blue/hud_arc_left_blue.svg
    resources/decor/blue/loading_bar_blue.svg
    resources/decor/blue/panel_frame_blue.svg
    resources/decor/blue/rail_vertical_blue.svg
    resources/decor/blue/side_strip_blue.svg
    resources/decor/blue/slashes_blue.svg
    resources/decor/green/badge_tag_green.svg
    resources/decor/green/corner_bottom_right_green.svg
    resources/decor/green/corner_top_left_green.svg
    resources/decor/green/divider_long_green.svg
    resources/decor/green/divider_notch_green.svg
    resources/decor/green/hud_arc_left_green.svg
    resources/decor/green/loading_bar_green.svg
    resources/decor/green/panel_frame_green.svg
    resources/decor/green/rail_vertical_green.svg
    resources/decor/green/side_strip_green.svg
    resources/decor/green/slashes_green.svg
    resources/decor/neutral/diagonal_lines_overlay.svg
    resources/decor/neutral/glow_dot.svg
    resources/decor/neutral/hex_overlay.svg
    resources/decor/neutral/status_error.svg
    resources/decor/neutral/status_offline.svg
    resources/decor/neutral/status_processing.svg
    resources/decor/neutral/status_ready.svg
)

if(WIN32)
  target_sources(xenon_launcher PRIVATE resources/windows/xenon_launcher.rc)
  set_source_files_properties(resources/windows/xenon_launcher.rc
    PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/windows/xenon-app-icon.ico")
  target_link_libraries(xenon_launcher_core PRIVATE dxgi)
endif()

target_link_libraries(xenon_launcher
  PRIVATE
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickDialogs2
    Xenon::LauncherCore
)

target_compile_features(xenon_launcher PRIVATE cxx_std_20)
target_compile_definitions(xenon_launcher PRIVATE
  XENON_LAUNCHER_VERSION="${XENON_LAUNCHER_VERSION_VALUE}"
  XENON_LAUNCHER_TEST_MODE=$<BOOL:${XENON_LAUNCHER_TEST_MODE}>
)
xenon_enable_warnings(xenon_launcher)

set_target_properties(xenon_launcher PROPERTIES
  WIN32_EXECUTABLE TRUE
  MACOSX_BUNDLE TRUE
)

install(TARGETS xenon_launcher
  BUNDLE DESTINATION .
  RUNTIME DESTINATION bin
)
