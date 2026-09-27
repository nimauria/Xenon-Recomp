# Deploy the Qt runtime required by a developer xenon_launcher build.
#
# This script is invoked by launcher/CMakeLists.txt as a POST_BUILD step.  It is
# intentionally separate from the installer deployment graph: the latter
# deploys into CMAKE_INSTALL_PREFIX, while this script makes the build-tree EXE
# directly runnable for developers and CI smoke tests.

foreach(_required IN ITEMS
    XENON_WINDEPLOYQT
    XENON_DEPLOY_CONFIG
    XENON_DEPLOY_QML_DIR
    XENON_DEPLOY_EXECUTABLE)
  if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
    message(FATAL_ERROR "DeployQtDeveloperRuntime: ${_required} was not provided")
  endif()
endforeach()

if(NOT EXISTS "${XENON_WINDEPLOYQT}")
  message(FATAL_ERROR
    "DeployQtDeveloperRuntime: windeployqt was not found: ${XENON_WINDEPLOYQT}")
endif()
if(NOT IS_DIRECTORY "${XENON_DEPLOY_QML_DIR}")
  message(FATAL_ERROR
    "DeployQtDeveloperRuntime: QML source directory was not found: ${XENON_DEPLOY_QML_DIR}")
endif()
if(NOT EXISTS "${XENON_DEPLOY_EXECUTABLE}")
  message(FATAL_ERROR
    "DeployQtDeveloperRuntime: launcher executable was not found: ${XENON_DEPLOY_EXECUTABLE}")
endif()

string(TOLOWER "${XENON_DEPLOY_CONFIG}" _xenon_deploy_config_lower)
if(_xenon_deploy_config_lower STREQUAL "debug")
  set(_xenon_deploy_mode --debug)
else()
  # RelWithDebInfo/MinSizeRel link against the release Qt runtime on the
  # official MSVC Qt packages, so deploy the release family for every
  # non-Debug configuration.
  set(_xenon_deploy_mode --release)
endif()

execute_process(
  COMMAND "${XENON_WINDEPLOYQT}"
    "${_xenon_deploy_mode}"
    --qmldir "${XENON_DEPLOY_QML_DIR}"
    --no-translations
    "${XENON_DEPLOY_EXECUTABLE}"
  RESULT_VARIABLE _xenon_deploy_result
  OUTPUT_VARIABLE _xenon_deploy_stdout
  ERROR_VARIABLE _xenon_deploy_stderr)

if(NOT _xenon_deploy_result EQUAL 0)
  message(FATAL_ERROR
    "windeployqt failed for ${XENON_DEPLOY_EXECUTABLE} (exit ${_xenon_deploy_result})\n"
    "stdout:\n${_xenon_deploy_stdout}\n"
    "stderr:\n${_xenon_deploy_stderr}")
endif()

message(STATUS
  "Qt/QML runtime deployed for ${XENON_DEPLOY_CONFIG}: ${XENON_DEPLOY_EXECUTABLE}")
