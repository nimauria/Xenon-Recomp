set(CEF_ROOT "" CACHE PATH "Extracted CEF binary distribution")
if(NOT EXISTS "${CEF_ROOT}/cmake/FindCEF.cmake")
  message(FATAL_ERROR "Set CEF_ROOT to a checksum-verified CEF binary distribution")
endif()
list(APPEND CMAKE_MODULE_PATH "${CEF_ROOT}/cmake")
find_package(CEF REQUIRED)
add_subdirectory("${CEF_LIBCEF_DLL_WRAPPER_PATH}" "${CMAKE_CURRENT_BINARY_DIR}/wrapper")
list(REMOVE_ITEM CEF_COMPILER_DEFINES _HAS_EXCEPTIONS=0)
if(APPLE)
  include(macos.cmake)
  return()
endif()
ADD_LOGICAL_TARGET(libcef_lib "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
if(WIN32)
  # Current CEF uses its supplied sandbox bootstrap to load our application DLL.
  add_library(xenon_launcher_next SHARED main.cpp)
  SET_LIBRARY_TARGET_PROPERTIES(xenon_launcher_next)
else()
  add_executable(xenon_launcher_next main.cpp)
  SET_EXECUTABLE_TARGET_PROPERTIES(xenon_launcher_next)
  set_target_properties(xenon_launcher_next PROPERTIES BUILD_RPATH "$ORIGIN")
endif()
target_link_libraries(xenon_launcher_next PRIVATE xenon_launcher_next_service libcef_lib libcef_dll_wrapper ${CEF_STANDARD_LIBS})
if(MSVC)
  target_compile_options(xenon_launcher_next PRIVATE /EHsc)
  target_compile_definitions(xenon_launcher_next PRIVATE _HAS_EXCEPTIONS=1)
else()
  # CEF's wrapper defaults disable C++ exceptions/RTTI. The application owns
  # JSON/filesystem error handling and must retain the standard C++ semantics.
  target_compile_options(xenon_launcher_next PRIVATE -fexceptions -frtti -fthreadsafe-statics)
endif()
target_include_directories(xenon_launcher_next PRIVATE "${CEF_ROOT}")
target_compile_definitions(xenon_launcher_next PRIVATE XENON_WEB_SOURCE="${PROJECT_SOURCE_DIR}/dist")
set_target_properties(xenon_launcher_next PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/desktop" LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/desktop")
add_custom_target(xenon_launcher_next_assets ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/dist" "$<TARGET_FILE_DIR:xenon_launcher_next>/web"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/contracts/settings.json" "$<TARGET_FILE_DIR:xenon_launcher_next>/settings-schema.json")
add_dependencies(xenon_launcher_next xenon_launcher_next_assets xenon_launcher_updater)
add_custom_command(TARGET xenon_launcher_next_assets POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/contracts/module-catalog-fallback.json" "$<TARGET_FILE_DIR:xenon_launcher_next>/module-catalog-fallback.json"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:xenon_launcher_next>/licenses"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/native/third_party/unicode/LICENSE.txt" "$<TARGET_FILE_DIR:xenon_launcher_next>/licenses/Unicode-LICENSE.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/native/third_party/picosha2/LICENSE" "$<TARGET_FILE_DIR:xenon_launcher_next>/licenses/PicoSHA2-LICENSE.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/native/third_party/miniz/LICENSE" "$<TARGET_FILE_DIR:xenon_launcher_next>/licenses/miniz-LICENSE.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/native/third_party/nlohmann/LICENSE.MIT" "$<TARGET_FILE_DIR:xenon_launcher_next>/licenses/nlohmann-JSON-LICENSE.txt")
foreach(asset IN LISTS CEF_BINARY_FILES)
  add_custom_command(TARGET xenon_launcher_next POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CEF_BINARY_DIR}/${asset}" "$<TARGET_FILE_DIR:xenon_launcher_next>/${asset}")
endforeach()
add_custom_command(TARGET xenon_launcher_next POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${CEF_RESOURCE_DIR}" "$<TARGET_FILE_DIR:xenon_launcher_next>"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/dist" "$<TARGET_FILE_DIR:xenon_launcher_next>/web"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/contracts/settings.json" "$<TARGET_FILE_DIR:xenon_launcher_next>/settings-schema.json"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CEF_ROOT}/LICENSE.txt" "$<TARGET_FILE_DIR:xenon_launcher_next>/CEF-LICENSE.txt")
if(WIN32)
  add_custom_command(TARGET xenon_launcher_next POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CEF_BINARY_DIR}/bootstrap.exe" "$<TARGET_FILE_DIR:xenon_launcher_next>/xenon_launcher_next.exe")
endif()
