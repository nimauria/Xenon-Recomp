enable_language(OBJCXX)
set(XENON_MAC_APP "${PROJECT_BINARY_DIR}/desktop/XenonLauncher.app")
function(xenon_mac_target target output principal agent bundle_id)
  SET_EXECUTABLE_TARGET_PROPERTIES(${target})
  target_include_directories(${target} PRIVATE "${CEF_ROOT}")
  target_link_libraries(${target} PRIVATE xenon_launcher_next_service libcef_dll_wrapper ${CEF_STANDARD_LIBS})
  target_compile_options(${target} PRIVATE -fexceptions -frtti -fthreadsafe-statics "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
  set(XENON_MAC_EXECUTABLE "${output}")
  set(XENON_MAC_BUNDLE_ID "${bundle_id}")
  set(XENON_MAC_PRINCIPAL_CLASS "${principal}")
  set(XENON_MAC_AGENT "${agent}")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/mac/Info.plist.in" "${CMAKE_CURRENT_BINARY_DIR}/${target}-Info.plist" @ONLY)
  set_target_properties(${target} PROPERTIES OUTPUT_NAME "${output}" RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/desktop"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${PROJECT_BINARY_DIR}/desktop" RUNTIME_OUTPUT_DIRECTORY_DEBUG "${PROJECT_BINARY_DIR}/desktop"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/${target}-Info.plist")
endfunction()
add_executable(xenon_launcher_next MACOSX_BUNDLE main.cpp mac/main.mm)
xenon_mac_target(xenon_launcher_next "XenonLauncher" "XenonApplication" false "org.xenon.launcher.next")
COPY_MAC_FRAMEWORK(xenon_launcher_next "${CEF_BINARY_DIR}" "${XENON_MAC_APP}")
foreach(role IN LISTS CEF_HELPER_APP_SUFFIXES)
  string(REPLACE ":" ";" fields "${role}")
  list(GET fields 0 name_suffix)
  list(GET fields 1 target_suffix)
  list(GET fields 2 bundle_suffix)
  set(helper "xenon_launcher_helper${target_suffix}")
  set(name "XenonLauncher Helper${name_suffix}")
  add_executable(${helper} MACOSX_BUNDLE main.cpp mac/helper.cpp)
  xenon_mac_target(${helper} "${name}" NSApplication true "org.xenon.launcher.next.helper${bundle_suffix}")
  add_dependencies(xenon_launcher_next ${helper})
  add_custom_command(TARGET xenon_launcher_next POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory
    "$<TARGET_BUNDLE_DIR:${helper}>" "${XENON_MAC_APP}/Contents/Frameworks/${name}.app" VERBATIM)
endforeach()
add_custom_target(xenon_launcher_next_assets ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory "${XENON_MAC_APP}/Contents/Resources/licenses"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/dist" "${XENON_MAC_APP}/Contents/Resources/web"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/contracts/settings.json" "${XENON_MAC_APP}/Contents/Resources/settings-schema.json"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/contracts/module-catalog-fallback.json" "${XENON_MAC_APP}/Contents/Resources/module-catalog-fallback.json"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CEF_ROOT}/LICENSE.txt" "${XENON_MAC_APP}/Contents/Resources/CEF-LICENSE.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/native/third_party/unicode/LICENSE.txt" "${XENON_MAC_APP}/Contents/Resources/licenses/Unicode-LICENSE.txt")
add_dependencies(xenon_launcher_next xenon_launcher_next_assets)
