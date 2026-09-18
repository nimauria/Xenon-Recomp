find_package(Qt6 6.6 QUIET COMPONENTS Gui Quick QuickControls2 QuickDialogs2)

if(NOT Qt6_FOUND)
  message(STATUS
    "Xenon launcher disabled: Qt 6.6+ Quick/QuickControls2/QuickDialogs2 was not found")
  return()
endif()

qt_standard_project_setup(REQUIRES 6.6)
if(QT_KNOWN_POLICY_QTP0004)
  qt_policy(SET QTP0004 NEW)
endif()

option(XENON_LAUNCHER_TEST_MODE
  "Populate the Xenon launcher with fictional front-end test data" OFF)

qt_add_executable(xenon_launcher
  src/main.cpp
  src/launcher_bridge.cpp
  src/launcher_bridge.hpp
  src/launcher_config.hpp
)

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
    qml/Sidebar.qml
    qml/FooterBar.qml
    qml/XPanel.qml
    qml/XButton.qml
    qml/XIconButton.qml
    qml/XWindowButton.qml
    qml/XenonBrand.qml
    qml/ProfileAvatar.qml
    qml/ModuleSettingsDialog.qml
    qml/ModuleCatalogDialog.qml
    qml/HelpPopover.qml
    qml/XNavButton.qml
    qml/XIcon.qml
    qml/XTextField.qml
    qml/XComboBox.qml
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
  target_link_libraries(xenon_launcher PRIVATE dxgi)
endif()

target_link_libraries(xenon_launcher
  PRIVATE
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickDialogs2
)

target_compile_features(xenon_launcher PRIVATE cxx_std_20)
target_compile_definitions(xenon_launcher PRIVATE
  XENON_LAUNCHER_VERSION="${PROJECT_VERSION}-dev"
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
