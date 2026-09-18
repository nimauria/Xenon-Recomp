find_package(Qt6 6.6 QUIET COMPONENTS Quick QuickControls2 QuickDialogs2)

if(NOT Qt6_FOUND)
  message(STATUS
    "Xenon launcher disabled: Qt 6.6+ Quick/QuickControls2/QuickDialogs2 was not found")
  return()
endif()

qt_standard_project_setup(REQUIRES 6.6)

qt_add_executable(xenon_launcher
  src/main.cpp
  src/launcher_bridge.cpp
  src/launcher_bridge.hpp
  src/launcher_config.hpp
)

set_source_files_properties(
  qml/Theme.qml
  PROPERTIES QT_QML_SINGLETON_TYPE TRUE
)

qt_add_qml_module(xenon_launcher
  URI Xenon.Launcher
  VERSION 1.0
  QML_FILES
    qml/Main.qml
    qml/Theme.qml
    qml/TopBar.qml
    qml/Sidebar.qml
    qml/FooterBar.qml
    qml/XPanel.qml
    qml/XButton.qml
    qml/StatusPill.qml
    qml/ArtworkFrame.qml
    qml/GameTile.qml
    qml/Toast.qml
    qml/EmptyState.qml
    qml/LibraryPage.qml
    qml/ModulesPage.qml
    qml/ProfilesPage.qml
    qml/SettingsPage.qml
)

target_link_libraries(xenon_launcher
  PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickDialogs2
)

target_compile_features(xenon_launcher PRIVATE cxx_std_20)
target_compile_definitions(xenon_launcher PRIVATE
  XENON_LAUNCHER_VERSION="${PROJECT_VERSION}-dev")
xenon_enable_warnings(xenon_launcher)

set_target_properties(xenon_launcher PROPERTIES
  WIN32_EXECUTABLE TRUE
  MACOSX_BUNDLE TRUE
)

install(TARGETS xenon_launcher
  BUNDLE DESTINATION .
  RUNTIME DESTINATION bin
)
