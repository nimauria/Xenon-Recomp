# Centralized third-party dependency resolution for Xenon.
#
# Xenon supports three dependency modes:
#   AUTO    - use explicit/managed dependencies first, then compatible system
#             packages, then bootstrap missing pinned dependencies.
#   MANAGED - use only explicit roots or Xenon's pinned managed dependencies.
#             Official release presets use this for reproducibility.
#   SYSTEM  - never bootstrap and never use Xenon's committed FFmpeg fallback;
#             only explicit roots or host-installed packages are accepted.
#
# Windows x64 keeps the committed static FFmpeg/XMA bundle as an AUTO-mode
# developer fallback. Managed release builds use a shared FFmpeg build staged
# beside Xenon binaries, which keeps LGPL redistribution straightforward.

include_guard(GLOBAL)

set(XENON_DEPENDENCY_MODE "AUTO" CACHE STRING
  "Dependency resolution mode: AUTO, MANAGED, or SYSTEM")
set_property(CACHE XENON_DEPENDENCY_MODE PROPERTY STRINGS AUTO MANAGED SYSTEM)
option(XENON_AUTO_BOOTSTRAP_DEPS
  "Automatically provision missing pinned dependencies during configure" ON)
# Backwards-compatible switch retained for existing build scripts. Both this
# and XENON_AUTO_BOOTSTRAP_DEPS must be ON before configure may invoke Python.
option(XENON_FETCH_MISSING_DEPS
  "Compatibility alias controlling automatic dependency provisioning" ON)
option(XENON_PREFER_BUNDLED_FFMPEG
  "Prefer Xenon's committed Windows x64 static FFmpeg bundle in AUTO mode" ON)
set(XENON_SDL2_ROOT "" CACHE PATH
  "Optional SDL2 installation root; overrides managed/system discovery")
set(XENON_AUDIO_FFMPEG_ROOT "" CACHE PATH
  "Optional FFmpeg root exposing AV_CODEC_ID_XMAFRAMES")
set(XENON_VULKAN_ROOT "" CACHE PATH
  "Optional Vulkan Loader installation root; release builds normally use the managed pinned loader")
set(XENON_VULKAN_HEADERS_ROOT "" CACHE PATH
  "Optional Vulkan-Headers installation root")
set(XENON_DXC_ROOT "" CACHE PATH
  "Optional DirectX Shader Compiler redistributable root")

if(WIN32)
  set(_xenon_dep_os "windows")
elseif(APPLE)
  set(_xenon_dep_os "macos")
elseif(UNIX)
  set(_xenon_dep_os "linux")
else()
  string(TOLOWER "${CMAKE_SYSTEM_NAME}" _xenon_dep_os)
endif()

set(_xenon_dep_processor "${CMAKE_SYSTEM_PROCESSOR}")
if(NOT _xenon_dep_processor)
  set(_xenon_dep_processor "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()
string(TOLOWER "${_xenon_dep_processor}" _xenon_dep_arch_raw)
if(_xenon_dep_arch_raw MATCHES "^(amd64|x86_64|x64)$")
  set(_xenon_dep_arch "x64")
elseif(_xenon_dep_arch_raw MATCHES "^(aarch64|arm64)$")
  set(_xenon_dep_arch "arm64")
elseif(_xenon_dep_arch_raw MATCHES "^(i[3-6]86|x86)$")
  set(_xenon_dep_arch "x86")
else()
  set(_xenon_dep_arch "${_xenon_dep_arch_raw}")
endif()

set(XENON_DEPENDENCY_TRIPLET "${_xenon_dep_os}-${_xenon_dep_arch}" CACHE STRING
  "Managed dependency platform/architecture triplet")
set(XENON_MANAGED_DEPS_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/.xenon/deps/${XENON_DEPENDENCY_TRIPLET}"
  CACHE PATH "Root containing Xenon's pinned managed dependencies")
set(XENON_DEPENDENCY_MANIFEST
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/deps/manifest.json")

string(TOUPPER "${XENON_DEPENDENCY_MODE}" _xenon_dep_mode_upper)
if(NOT _xenon_dep_mode_upper MATCHES "^(AUTO|MANAGED|SYSTEM)$")
  message(FATAL_ERROR
    "XENON_DEPENDENCY_MODE must be AUTO, MANAGED, or SYSTEM (got '${XENON_DEPENDENCY_MODE}')")
endif()
set(XENON_DEPENDENCY_MODE "${_xenon_dep_mode_upper}" CACHE STRING
  "Dependency resolution mode: AUTO, MANAGED, or SYSTEM" FORCE)

if(NOT EXISTS "${XENON_DEPENDENCY_MANIFEST}")
  message(FATAL_ERROR
    "Xenon dependency manifest is missing: ${XENON_DEPENDENCY_MANIFEST}")
endif()
file(READ "${XENON_DEPENDENCY_MANIFEST}" _xenon_dependency_manifest_json)
file(SHA256 "${XENON_DEPENDENCY_MANIFEST}" _xenon_dependency_manifest_sha256)
string(JSON _xenon_manifest_schema ERROR_VARIABLE _xenon_manifest_schema_error
  GET "${_xenon_dependency_manifest_json}" schema)
if(_xenon_manifest_schema_error OR NOT _xenon_manifest_schema EQUAL 3)
  message(FATAL_ERROR
    "Unsupported Xenon dependency manifest schema in ${XENON_DEPENDENCY_MANIFEST}")
endif()

function(_xenon_manifest_value dependency_name field_name out_var)
  string(JSON _value ERROR_VARIABLE _json_error GET
    "${_xenon_dependency_manifest_json}" dependencies "${dependency_name}" "${field_name}")
  if(_json_error)
    message(FATAL_ERROR
      "Invalid Xenon dependency manifest entry '${dependency_name}.${field_name}': ${_json_error}")
  endif()
  set(${out_var} "${_value}" PARENT_SCOPE)
endfunction()

function(_xenon_managed_prefix dependency_name out_var)
  _xenon_manifest_value("${dependency_name}" install_subdir _install_subdir)
  set(${out_var} "${XENON_MANAGED_DEPS_ROOT}/${_install_subdir}" PARENT_SCOPE)
endfunction()

function(_xenon_managed_dependency_ready dependency_name out_var)
  _xenon_managed_prefix("${dependency_name}" _prefix)
  set(_stamp "${_prefix}/.xenon-dependency.json")
  if(NOT EXISTS "${_stamp}")
    set(${out_var} FALSE PARENT_SCOPE)
    return()
  endif()

  file(READ "${_stamp}" _stamp_json)
  string(JSON _stamp_schema ERROR_VARIABLE _stamp_schema_error GET "${_stamp_json}" schema)
  string(JSON _stamp_revision ERROR_VARIABLE _stamp_revision_error GET "${_stamp_json}" revision)
  string(JSON _stamp_triplet ERROR_VARIABLE _stamp_triplet_error GET "${_stamp_json}" triplet)
  string(JSON _stamp_manifest ERROR_VARIABLE _stamp_manifest_error GET "${_stamp_json}" manifest_sha256)
  _xenon_manifest_value("${dependency_name}" revision _expected_revision)

  if(_stamp_schema_error OR _stamp_revision_error OR _stamp_triplet_error OR _stamp_manifest_error OR
     NOT _stamp_schema EQUAL 3 OR
     NOT _stamp_revision STREQUAL _expected_revision OR
     NOT _stamp_triplet STREQUAL XENON_DEPENDENCY_TRIPLET OR
     NOT _stamp_manifest STREQUAL _xenon_dependency_manifest_sha256)
    set(${out_var} FALSE PARENT_SCOPE)
    return()
  endif()
  set(${out_var} TRUE PARENT_SCOPE)
endfunction()

function(_xenon_bootstrap_dependency dependency_name out_result)
  if(NOT XENON_AUTO_BOOTSTRAP_DEPS OR NOT XENON_FETCH_MISSING_DEPS)
    set(${out_result} 1 PARENT_SCOPE)
    return()
  endif()
  if(CMAKE_CROSSCOMPILING)
    message(STATUS
      "Xenon dependency bootstrap skipped while cross-compiling; provision '${dependency_name}' for ${XENON_DEPENDENCY_TRIPLET} explicitly")
    set(${out_result} 1 PARENT_SCOPE)
    return()
  endif()

  find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
  if(NOT Python3_Interpreter_FOUND)
    message(STATUS
      "Python 3.9+ is unavailable, so Xenon cannot auto-bootstrap '${dependency_name}'")
    set(${out_result} 1 PARENT_SCOPE)
    return()
  endif()

  message(STATUS
    "Provisioning Xenon managed dependency '${dependency_name}' for ${XENON_DEPENDENCY_TRIPLET}")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/deps/bootstrap.py"
      ensure
      --triplet "${XENON_DEPENDENCY_TRIPLET}"
      --root "${XENON_MANAGED_DEPS_ROOT}"
      --only "${dependency_name}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE _bootstrap_result
    COMMAND_ECHO STDOUT)
  set(${out_result} "${_bootstrap_result}" PARENT_SCOPE)
endfunction()

function(_xenon_select_sdl2_target out_target root_path allow_system)
  set(_target "")

  if(root_path)
    find_package(SDL2 2.0.9 QUIET CONFIG
      PATHS "${root_path}" "${root_path}/lib/cmake/SDL2" "${root_path}/lib64/cmake/SDL2"
      NO_DEFAULT_PATH)
  elseif(allow_system)
    find_package(SDL2 2.0.9 QUIET CONFIG)
    if(NOT TARGET SDL2::SDL2 AND NOT TARGET SDL2::SDL2-static)
      find_package(SDL2 2.0.9 QUIET)
    endif()
  endif()

  if(TARGET SDL2::SDL2-static)
    set(_target SDL2::SDL2-static)
  elseif(TARGET SDL2::SDL2)
    set(_target SDL2::SDL2)
  elseif(allow_system AND SDL2_FOUND AND SDL2_LIBRARIES)
    if(NOT TARGET xenon_sdl2_system_external)
      add_library(xenon_sdl2_system_external INTERFACE)
      target_include_directories(xenon_sdl2_system_external INTERFACE ${SDL2_INCLUDE_DIRS})
      target_link_libraries(xenon_sdl2_system_external INTERFACE ${SDL2_LIBRARIES})
    endif()
    set(_target xenon_sdl2_system_external)
  elseif(root_path)
    find_path(_xenon_sdl2_include SDL.h
      HINTS "${root_path}/include" "${root_path}/include/SDL2"
      PATH_SUFFIXES SDL2
      NO_DEFAULT_PATH NO_CACHE)
    find_library(_xenon_sdl2_library
      NAMES SDL2-static SDL2 libSDL2
      HINTS "${root_path}/lib" "${root_path}/lib64"
      NO_DEFAULT_PATH NO_CACHE)
    if(_xenon_sdl2_include AND _xenon_sdl2_library)
      string(MD5 _sdl_target_suffix "${root_path};${_xenon_sdl2_include};${_xenon_sdl2_library}")
      set(_candidate "xenon_sdl2_external_${_sdl_target_suffix}")
      if(NOT TARGET ${_candidate})
        add_library(${_candidate} INTERFACE)
        target_include_directories(${_candidate} INTERFACE "${_xenon_sdl2_include}")
        target_link_libraries(${_candidate} INTERFACE "${_xenon_sdl2_library}")
      endif()
      set(_target ${_candidate})
    endif()
  endif()

  set(${out_target} "${_target}" PARENT_SCOPE)
endfunction()

function(xenon_resolve_sdl2 out_target)
  _xenon_managed_prefix("sdl2" _managed_root)
  _xenon_managed_dependency_ready("sdl2" _managed_ready)
  set(_target "")

  if(XENON_SDL2_ROOT)
    _xenon_select_sdl2_target(_target "${XENON_SDL2_ROOT}" FALSE)
  elseif(NOT XENON_DEPENDENCY_MODE STREQUAL "SYSTEM" AND _managed_ready)
    _xenon_select_sdl2_target(_target "${_managed_root}" FALSE)
  endif()

  if(NOT _target AND (XENON_DEPENDENCY_MODE STREQUAL "AUTO" OR
                      XENON_DEPENDENCY_MODE STREQUAL "SYSTEM"))
    _xenon_select_sdl2_target(_target "" TRUE)
  endif()

  if(NOT _target AND NOT XENON_SDL2_ROOT AND
     NOT XENON_DEPENDENCY_MODE STREQUAL "SYSTEM")
    _xenon_bootstrap_dependency("sdl2" _bootstrap_result)
    if(_bootstrap_result EQUAL 0)
      _xenon_managed_dependency_ready("sdl2" _managed_ready)
      if(_managed_ready)
        unset(SDL2_DIR CACHE)
        _xenon_select_sdl2_target(_target "${_managed_root}" FALSE)
      endif()
    endif()
  endif()

  set(${out_target} "${_target}" PARENT_SCOPE)
endfunction()

function(_xenon_ffmpeg_include_has_xmaframes include_dir out_var)
  set(_codec_id "${include_dir}/libavcodec/codec_id.h")
  if(NOT EXISTS "${_codec_id}")
    set(${out_var} FALSE PARENT_SCOPE)
    return()
  endif()
  file(READ "${_codec_id}" _codec_id_text)
  string(FIND "${_codec_id_text}" "AV_CODEC_ID_XMAFRAMES" _xma_pos)
  if(_xma_pos EQUAL -1)
    set(${out_var} FALSE PARENT_SCOPE)
  else()
    set(${out_var} TRUE PARENT_SCOPE)
  endif()
endfunction()

function(_xenon_make_ffmpeg_external_target out_target out_include root_path allow_system)
  set(_target "")
  unset(_include)
  unset(_ffmpeg_avcodec)
  unset(_ffmpeg_avutil)

  if(root_path)
    find_path(_include libavcodec/avcodec.h
      HINTS "${root_path}" "${root_path}/include"
      NO_DEFAULT_PATH NO_CACHE)
    find_library(_ffmpeg_avcodec NAMES avcodec libavcodec
      HINTS "${root_path}/lib" "${root_path}/lib64"
      NO_DEFAULT_PATH NO_CACHE)
    find_library(_ffmpeg_avutil NAMES avutil libavutil
      HINTS "${root_path}/lib" "${root_path}/lib64"
      NO_DEFAULT_PATH NO_CACHE)
  elseif(allow_system)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(XENON_FFMPEG_SYSTEM QUIET IMPORTED_TARGET libavcodec libavutil)
    endif()
    if(TARGET PkgConfig::XENON_FFMPEG_SYSTEM)
      get_target_property(_pkg_includes PkgConfig::XENON_FFMPEG_SYSTEM INTERFACE_INCLUDE_DIRECTORIES)
      foreach(_candidate_include IN LISTS _pkg_includes)
        if(EXISTS "${_candidate_include}/libavcodec/avcodec.h")
          set(_include "${_candidate_include}")
          break()
        endif()
      endforeach()
      _xenon_ffmpeg_include_has_xmaframes("${_include}" _pkg_has_xmaframes)
      if(_pkg_has_xmaframes)
        set(_target PkgConfig::XENON_FFMPEG_SYSTEM)
      endif()
    endif()
    if(NOT _target)
      find_path(_include libavcodec/avcodec.h NO_CACHE)
      find_library(_ffmpeg_avcodec NAMES avcodec libavcodec NO_CACHE)
      find_library(_ffmpeg_avutil NAMES avutil libavutil NO_CACHE)
    endif()
  endif()

  if(NOT _target AND _include AND _ffmpeg_avcodec AND _ffmpeg_avutil)
    _xenon_ffmpeg_include_has_xmaframes("${_include}" _has_xmaframes)
    if(_has_xmaframes)
      string(MD5 _ffmpeg_target_suffix
        "${root_path};${_include};${_ffmpeg_avcodec};${_ffmpeg_avutil}")
      set(_candidate "xenon_ffmpeg_external_${_ffmpeg_target_suffix}")
      if(NOT TARGET ${_candidate})
        add_library(${_candidate} INTERFACE)
        target_include_directories(${_candidate} INTERFACE "${_include}")
        target_link_libraries(${_candidate} INTERFACE
          "${_ffmpeg_avcodec}" "${_ffmpeg_avutil}")
        if(WIN32)
          target_link_libraries(${_candidate} INTERFACE bcrypt)
        elseif(UNIX AND NOT APPLE)
          target_link_libraries(${_candidate} INTERFACE m pthread dl)
        endif()
      endif()
      set(_target ${_candidate})
    endif()
  endif()

  if(NOT DEFINED _include)
    set(_include "")
  endif()
  set(${out_target} "${_target}" PARENT_SCOPE)
  set(${out_include} "${_include}" PARENT_SCOPE)
endfunction()

function(_xenon_resolve_bundled_windows_ffmpeg out_target out_include)
  set(_target "")
  set(_include "")
  if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(${out_target} "" PARENT_SCOPE)
    set(${out_include} "" PARENT_SCOPE)
    return()
  endif()

  set(_root "${PROJECT_SOURCE_DIR}/third_party/xenon-ffmpeg/windows-x64")
  set(_manifest "${_root}/XenonFFmpegBundle.cmake")
  if(NOT EXISTS "${_manifest}" OR
     NOT EXISTS "${_root}/include/libavcodec/avcodec.h" OR
     NOT EXISTS "${_root}/include/libavcodec/codec_id.h" OR
     NOT EXISTS "${_root}/include/libavutil/avutil.h" OR
     NOT EXISTS "${_root}/lib/libavcodec.lib" OR
     NOT EXISTS "${_root}/lib/libavutil.lib")
    set(${out_target} "" PARENT_SCOPE)
    set(${out_include} "" PARENT_SCOPE)
    return()
  endif()

  include("${_manifest}")
  _xenon_manifest_value("xenia-ffmpeg" revision _expected_ffmpeg_revision)
  if(NOT XENON_FFMPEG_BUNDLE_SCHEMA EQUAL 2 OR
     NOT XENON_FFMPEG_BUNDLE_FFMPEG_REVISION STREQUAL _expected_ffmpeg_revision OR
     NOT XENON_FFMPEG_BUNDLE_TRIPLET STREQUAL "windows-x64" OR
     NOT XENON_FFMPEG_BUNDLE_LINKAGE STREQUAL "static")
    message(FATAL_ERROR
      "Bundled Xenon FFmpeg/XMA identity metadata is stale or incompatible. Rebuild it with tools/rebuild_xenon_ffmpeg.ps1.")
  endif()

  file(SHA256 "${_root}/lib/libavcodec.lib" _bundle_avcodec_sha256)
  file(SHA256 "${_root}/lib/libavutil.lib" _bundle_avutil_sha256)
  file(SHA256 "${_root}/include/libavcodec/codec_id.h" _bundle_codec_id_sha256)
  if(NOT _bundle_avcodec_sha256 STREQUAL XENON_FFMPEG_BUNDLE_AVCODEC_SHA256 OR
     NOT _bundle_avutil_sha256 STREQUAL XENON_FFMPEG_BUNDLE_AVUTIL_SHA256 OR
     NOT _bundle_codec_id_sha256 STREQUAL XENON_FFMPEG_BUNDLE_CODEC_ID_SHA256)
    message(FATAL_ERROR
      "Bundled Xenon FFmpeg/XMA failed its SHA-256 identity check. Rebuild it with tools/rebuild_xenon_ffmpeg.ps1 or restore the committed bundle.")
  endif()

  if(NOT TARGET xenon_ffmpeg_avutil_bundled)
    add_library(xenon_ffmpeg_avutil_bundled STATIC IMPORTED GLOBAL)
    set_target_properties(xenon_ffmpeg_avutil_bundled PROPERTIES
      IMPORTED_LOCATION "${_root}/lib/libavutil.lib"
      INTERFACE_INCLUDE_DIRECTORIES "${_root}/include")
  endif()
  if(NOT TARGET xenon_ffmpeg_avcodec_bundled)
    add_library(xenon_ffmpeg_avcodec_bundled STATIC IMPORTED GLOBAL)
    set_target_properties(xenon_ffmpeg_avcodec_bundled PROPERTIES
      IMPORTED_LOCATION "${_root}/lib/libavcodec.lib"
      INTERFACE_INCLUDE_DIRECTORIES "${_root}/include")
    target_link_libraries(xenon_ffmpeg_avcodec_bundled INTERFACE
      xenon_ffmpeg_avutil_bundled bcrypt)
  endif()
  if(NOT TARGET xenon_audio_ffmpeg_bundled)
    add_library(xenon_audio_ffmpeg_bundled INTERFACE)
    target_link_libraries(xenon_audio_ffmpeg_bundled INTERFACE
      xenon_ffmpeg_avcodec_bundled)
  endif()

  set(_target xenon_audio_ffmpeg_bundled)
  set(_include "${_root}/include")
  set(${out_target} "${_target}" PARENT_SCOPE)
  set(${out_include} "${_include}" PARENT_SCOPE)
endfunction()

function(_xenon_record_ffmpeg_runtime root_path)
  if(EXISTS "${root_path}/runtime")
    set_property(GLOBAL PROPERTY XENON_FFMPEG_RUNTIME_DIR "${root_path}/runtime")
  endif()
  if(EXISTS "${root_path}/licenses/xenia-ffmpeg")
    set_property(GLOBAL PROPERTY XENON_FFMPEG_LICENSE_DIR "${root_path}/licenses/xenia-ffmpeg")
  elseif(EXISTS "${root_path}/licenses")
    set_property(GLOBAL PROPERTY XENON_FFMPEG_LICENSE_DIR "${root_path}/licenses")
  endif()
endfunction()

function(xenon_resolve_audio_ffmpeg out_target out_include out_bundled)
  _xenon_managed_prefix("xenia-ffmpeg" _managed_root)
  _xenon_managed_dependency_ready("xenia-ffmpeg" _managed_ready)
  set(_target "")
  set(_include "")
  set(_bundled FALSE)

  # Explicit roots always win. They are still capability-probed by the Audio
  # CMake block before Xenon accepts them.
  if(XENON_AUDIO_FFMPEG_ROOT)
    _xenon_make_ffmpeg_external_target(_target _include
      "${XENON_AUDIO_FFMPEG_ROOT}" FALSE)
    if(_target)
      _xenon_record_ffmpeg_runtime("${XENON_AUDIO_FFMPEG_ROOT}")
    endif()
  endif()

  # Reuse an already provisioned managed shared build before falling back to a
  # static bundle or system package.
  if(NOT _target AND NOT XENON_DEPENDENCY_MODE STREQUAL "SYSTEM" AND _managed_ready)
    _xenon_make_ffmpeg_external_target(_target _include "${_managed_root}" FALSE)
    if(_target)
      _xenon_record_ffmpeg_runtime("${_managed_root}")
    endif()
  endif()

  # Preserve the current repository's self-contained Windows developer path.
  # Official release presets use MANAGED and therefore deliberately skip this.
  if(NOT _target AND XENON_DEPENDENCY_MODE STREQUAL "AUTO" AND
     XENON_PREFER_BUNDLED_FFMPEG)
    _xenon_resolve_bundled_windows_ffmpeg(_target _include)
    if(_target)
      set(_bundled TRUE)
      set_property(GLOBAL PROPERTY XENON_FFMPEG_LICENSE_DIR
        "${PROJECT_SOURCE_DIR}/third_party/xenon-ffmpeg/windows-x64/licenses")
      message(STATUS "Using committed Xenon Windows x64 FFmpeg/XMA developer bundle")
    endif()
  endif()

  if(NOT _target AND (XENON_DEPENDENCY_MODE STREQUAL "AUTO" OR
                      XENON_DEPENDENCY_MODE STREQUAL "SYSTEM"))
    _xenon_make_ffmpeg_external_target(_target _include "" TRUE)
  endif()

  if(NOT _target AND NOT XENON_AUDIO_FFMPEG_ROOT AND
     NOT XENON_DEPENDENCY_MODE STREQUAL "SYSTEM")
    _xenon_bootstrap_dependency("xenia-ffmpeg" _bootstrap_result)
    if(_bootstrap_result EQUAL 0)
      _xenon_managed_dependency_ready("xenia-ffmpeg" _managed_ready)
      if(_managed_ready)
        _xenon_make_ffmpeg_external_target(_target _include "${_managed_root}" FALSE)
        if(_target)
          _xenon_record_ffmpeg_runtime("${_managed_root}")
        endif()
      endif()
    endif()
  endif()

  set(${out_target} "${_target}" PARENT_SCOPE)
  set(${out_include} "${_include}" PARENT_SCOPE)
  set(${out_bundled} "${_bundled}" PARENT_SCOPE)
endfunction()

function(_xenon_record_dependency_runtime property_name root_path license_subdir)
  if(EXISTS "${root_path}/runtime")
    set_property(GLOBAL PROPERTY ${property_name} "${root_path}/runtime")
  endif()
  if(EXISTS "${root_path}/licenses/${license_subdir}")
    set_property(GLOBAL PROPERTY ${property_name}_LICENSE "${root_path}/licenses/${license_subdir}")
  elseif(EXISTS "${root_path}/licenses")
    set_property(GLOBAL PROPERTY ${property_name}_LICENSE "${root_path}/licenses")
  endif()
endfunction()

function(_xenon_make_dxc_target out_target out_include root_path allow_system)
  set(_target "")
  unset(_include)
  unset(_library)
  if(root_path)
    find_path(_include dxc/dxcapi.h HINTS "${root_path}/include" "${root_path}" NO_DEFAULT_PATH NO_CACHE)
    find_library(_library NAMES dxcompiler libdxcompiler HINTS "${root_path}/lib" "${root_path}/lib64" NO_DEFAULT_PATH NO_CACHE)
  elseif(allow_system)
    if(DEFINED ENV{VULKAN_SDK})
      find_path(_include dxc/dxcapi.h PATHS "$ENV{VULKAN_SDK}/Include" NO_DEFAULT_PATH NO_CACHE)
      find_library(_library NAMES dxcompiler PATHS "$ENV{VULKAN_SDK}/Lib" NO_DEFAULT_PATH NO_CACHE)
    endif()
    if(NOT _include)
      find_path(_include dxc/dxcapi.h NO_CACHE)
    endif()
    if(NOT _library)
      find_library(_library NAMES dxcompiler libdxcompiler NO_CACHE)
    endif()
  endif()
  if(_include AND _library)
    string(MD5 _suffix "${root_path};${_include};${_library}")
    set(_candidate "xenon_dxc_external_${_suffix}")
    if(NOT TARGET ${_candidate})
      add_library(${_candidate} INTERFACE)
      target_include_directories(${_candidate} INTERFACE "${_include}")
      target_link_libraries(${_candidate} INTERFACE "${_library}")
      if(UNIX AND NOT APPLE)
        target_link_libraries(${_candidate} INTERFACE dl pthread)
      endif()
    endif()
    set(_target ${_candidate})
  endif()
  set(${out_target} "${_target}" PARENT_SCOPE)
  set(${out_include} "${_include}" PARENT_SCOPE)
endfunction()

function(xenon_resolve_dxc out_target out_include)
  _xenon_managed_prefix("dxc" _managed_root)
  _xenon_managed_dependency_ready("dxc" _managed_ready)
  set(_target "")
  set(_include "")

  if(XENON_DXC_ROOT)
    _xenon_make_dxc_target(_target _include "${XENON_DXC_ROOT}" FALSE)
    if(_target)
      _xenon_record_dependency_runtime(XENON_DXC_RUNTIME_DIR "${XENON_DXC_ROOT}" dxc)
    endif()
  elseif(NOT XENON_DEPENDENCY_MODE STREQUAL "SYSTEM" AND _managed_ready)
    _xenon_make_dxc_target(_target _include "${_managed_root}" FALSE)
    if(_target)
      _xenon_record_dependency_runtime(XENON_DXC_RUNTIME_DIR "${_managed_root}" dxc)
    endif()
  endif()

  if(NOT _target AND (XENON_DEPENDENCY_MODE STREQUAL "AUTO" OR XENON_DEPENDENCY_MODE STREQUAL "SYSTEM"))
    _xenon_make_dxc_target(_target _include "" TRUE)
    if(_target AND WIN32 AND DEFINED ENV{VULKAN_SDK} AND
       EXISTS "$ENV{VULKAN_SDK}/Bin/dxcompiler.dll")
      # The Vulkan SDK's own dxcompiler.dll is a real runtime dependency of
      # this resolution path (its shader-translation API calls straight into
      # it), but it lives under the SDK's `Bin/` layout (alongside dozens of
      # unrelated SDK tools/validation-layer DLLs), not the curated `runtime/`
      # layout `_xenon_record_dependency_runtime()` expects from a
      # Xenon-managed dependency root - so this system fallback previously
      # never staged anything, leaving every GPU/shader test that actually
      # invokes SPIR-V codegen to fail with "SPIR-V CodeGen not available"
      # despite a working DXC being installed and found at configure time.
      # Stage just the one required DLL into a curated directory (never the
      # whole SDK Bin folder - xenon_stage_runtime_dependencies() copies
      # every file in the recorded directory verbatim next to each target)
      # and point the existing runtime-dependency copy mechanism at that.
      set(_dxc_staged_runtime_dir "${CMAKE_BINARY_DIR}/_xenon_runtime/dxc")
      file(MAKE_DIRECTORY "${_dxc_staged_runtime_dir}")
      file(COPY_FILE "$ENV{VULKAN_SDK}/Bin/dxcompiler.dll"
        "${_dxc_staged_runtime_dir}/dxcompiler.dll" ONLY_IF_DIFFERENT)
      if(EXISTS "$ENV{VULKAN_SDK}/Bin/dxil.dll")
        file(COPY_FILE "$ENV{VULKAN_SDK}/Bin/dxil.dll"
          "${_dxc_staged_runtime_dir}/dxil.dll" ONLY_IF_DIFFERENT)
      endif()
      set_property(GLOBAL PROPERTY XENON_DXC_RUNTIME_DIR "${_dxc_staged_runtime_dir}")
    endif()
  endif()

  if(NOT _target AND NOT XENON_DXC_ROOT AND NOT XENON_DEPENDENCY_MODE STREQUAL "SYSTEM")
    _xenon_bootstrap_dependency("dxc" _bootstrap_result)
    if(_bootstrap_result EQUAL 0)
      _xenon_managed_dependency_ready("dxc" _managed_ready)
      if(_managed_ready)
        _xenon_make_dxc_target(_target _include "${_managed_root}" FALSE)
        if(_target)
          _xenon_record_dependency_runtime(XENON_DXC_RUNTIME_DIR "${_managed_root}" dxc)
        endif()
      endif()
    endif()
  endif()
  set(${out_target} "${_target}" PARENT_SCOPE)
  set(${out_include} "${_include}" PARENT_SCOPE)
endfunction()

function(_xenon_make_vulkan_target out_target loader_root headers_root allow_system)
  set(_target "")
  unset(_include)
  unset(_library)
  if(loader_root OR headers_root)
    find_path(_include vulkan/vulkan.h HINTS "${headers_root}/include" "${loader_root}/include" NO_DEFAULT_PATH NO_CACHE)
    find_library(_library NAMES vulkan-1 vulkan libvulkan HINTS "${loader_root}/lib" "${loader_root}/lib64" NO_DEFAULT_PATH NO_CACHE)
  elseif(allow_system)
    find_package(Vulkan QUIET)
    if(TARGET Vulkan::Vulkan)
      set(${out_target} Vulkan::Vulkan PARENT_SCOPE)
      return()
    endif()
  endif()
  if(_include AND _library)
    string(MD5 _suffix "${loader_root};${headers_root};${_include};${_library}")
    set(_candidate "xenon_vulkan_external_${_suffix}")
    if(NOT TARGET ${_candidate})
      add_library(${_candidate} INTERFACE)
      target_include_directories(${_candidate} INTERFACE "${_include}")
      target_link_libraries(${_candidate} INTERFACE "${_library}")
      if(UNIX AND NOT APPLE)
        target_link_libraries(${_candidate} INTERFACE dl pthread)
      endif()
    endif()
    set(_target ${_candidate})
  endif()
  set(${out_target} "${_target}" PARENT_SCOPE)
endfunction()

function(xenon_resolve_vulkan out_target)
  _xenon_managed_prefix("vulkan-headers" _managed_headers)
  _xenon_managed_prefix("vulkan-loader" _managed_loader)
  _xenon_managed_dependency_ready("vulkan-headers" _headers_ready)
  _xenon_managed_dependency_ready("vulkan-loader" _loader_ready)
  set(_target "")

  if(XENON_VULKAN_ROOT OR XENON_VULKAN_HEADERS_ROOT)
    set(_headers_root "${XENON_VULKAN_HEADERS_ROOT}")
    if(NOT _headers_root)
      set(_headers_root "${XENON_VULKAN_ROOT}")
    endif()
    _xenon_make_vulkan_target(_target "${XENON_VULKAN_ROOT}" "${_headers_root}" FALSE)
    if(_target AND XENON_VULKAN_ROOT)
      _xenon_record_dependency_runtime(XENON_VULKAN_RUNTIME_DIR "${XENON_VULKAN_ROOT}" vulkan-loader)
    endif()
  elseif(NOT XENON_DEPENDENCY_MODE STREQUAL "SYSTEM" AND _headers_ready AND _loader_ready)
    _xenon_make_vulkan_target(_target "${_managed_loader}" "${_managed_headers}" FALSE)
    if(_target)
      _xenon_record_dependency_runtime(XENON_VULKAN_RUNTIME_DIR "${_managed_loader}" vulkan-loader)
    endif()
  endif()

  if(NOT _target AND (XENON_DEPENDENCY_MODE STREQUAL "AUTO" OR XENON_DEPENDENCY_MODE STREQUAL "SYSTEM"))
    _xenon_make_vulkan_target(_target "" "" TRUE)
  endif()

  if(NOT _target AND NOT XENON_VULKAN_ROOT AND NOT XENON_DEPENDENCY_MODE STREQUAL "SYSTEM")
    _xenon_bootstrap_dependency("vulkan-loader" _bootstrap_result)
    if(_bootstrap_result EQUAL 0)
      _xenon_managed_dependency_ready("vulkan-headers" _headers_ready)
      _xenon_managed_dependency_ready("vulkan-loader" _loader_ready)
      if(_headers_ready AND _loader_ready)
        _xenon_make_vulkan_target(_target "${_managed_loader}" "${_managed_headers}" FALSE)
        if(_target)
          _xenon_record_dependency_runtime(XENON_VULKAN_RUNTIME_DIR "${_managed_loader}" vulkan-loader)
        endif()
      endif()
    endif()
  endif()
  set(${out_target} "${_target}" PARENT_SCOPE)
endfunction()

function(xenon_stage_runtime_dependencies target_name)
  if(NOT TARGET ${target_name})
    message(FATAL_ERROR "xenon_stage_runtime_dependencies: unknown target '${target_name}'")
  endif()
  foreach(_property IN ITEMS XENON_FFMPEG_RUNTIME_DIR XENON_DXC_RUNTIME_DIR XENON_VULKAN_RUNTIME_DIR)
    get_property(_runtime_dir GLOBAL PROPERTY ${_property})
    if(_runtime_dir AND EXISTS "${_runtime_dir}")
      file(GLOB _runtime_files LIST_DIRECTORIES FALSE "${_runtime_dir}/*")
      foreach(_runtime_file IN LISTS _runtime_files)
        add_custom_command(TARGET ${target_name} POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_runtime_file}" "$<TARGET_FILE_DIR:${target_name}>"
          VERBATIM)
      endforeach()
    endif()
  endforeach()
  if(UNIX AND NOT APPLE)
    set_property(TARGET ${target_name} APPEND PROPERTY BUILD_RPATH "$ORIGIN")
    set_property(TARGET ${target_name} APPEND PROPERTY INSTALL_RPATH "$ORIGIN")
  elseif(APPLE)
    set_property(TARGET ${target_name} APPEND PROPERTY BUILD_RPATH "@loader_path")
    set_property(TARGET ${target_name} APPEND PROPERTY INSTALL_RPATH "@loader_path")
  endif()
endfunction()

function(xenon_install_runtime_dependencies runtime_destination license_destination)
  foreach(_property IN ITEMS XENON_FFMPEG_RUNTIME_DIR XENON_DXC_RUNTIME_DIR XENON_VULKAN_RUNTIME_DIR)
    get_property(_runtime_dir GLOBAL PROPERTY ${_property})
    if(_runtime_dir AND EXISTS "${_runtime_dir}")
      install(DIRECTORY "${_runtime_dir}/" DESTINATION "${runtime_destination}")
    endif()
  endforeach()
  foreach(_pair IN ITEMS
      "XENON_FFMPEG_LICENSE_DIR|xenia-ffmpeg"
      "XENON_DXC_RUNTIME_DIR_LICENSE|dxc"
      "XENON_VULKAN_RUNTIME_DIR_LICENSE|vulkan-loader")
    string(REPLACE "|" ";" _fields "${_pair}")
    list(GET _fields 0 _property)
    list(GET _fields 1 _name)
    get_property(_license_dir GLOBAL PROPERTY ${_property})
    if(_license_dir AND EXISTS "${_license_dir}")
      install(DIRECTORY "${_license_dir}/" DESTINATION "${license_destination}/${_name}")
    endif()
  endforeach()
  foreach(_dep IN ITEMS sdl2 vulkan-headers)
    _xenon_managed_prefix("${_dep}" _prefix)
    if(EXISTS "${_prefix}/licenses/${_dep}")
      install(DIRECTORY "${_prefix}/licenses/${_dep}/" DESTINATION "${license_destination}/${_dep}")
    elseif(EXISTS "${_prefix}/licenses")
      install(DIRECTORY "${_prefix}/licenses/" DESTINATION "${license_destination}/${_dep}")
    endif()
  endforeach()
endfunction()

function(xenon_dependency_help dependency_name)
  if(dependency_name STREQUAL "sdl2")
    message(STATUS "Run: python tools/deps/bootstrap.py ensure --only sdl2")
  elseif(dependency_name STREQUAL "xenia-ffmpeg")
    message(STATUS "Run: python tools/deps/bootstrap.py ensure --only xenia-ffmpeg")
  elseif(dependency_name STREQUAL "dxc")
    message(STATUS "Run: python tools/deps/bootstrap.py ensure --only dxc")
  elseif(dependency_name STREQUAL "vulkan-loader")
    message(STATUS "Run: python tools/deps/bootstrap.py ensure --only vulkan-loader")
  else()
    message(STATUS "Run: python tools/deps/bootstrap.py ensure")
  endif()
endfunction()

unset(_xenon_dep_os)
unset(_xenon_dep_arch)
unset(_xenon_dep_arch_raw)
unset(_xenon_dep_processor)
unset(_xenon_dep_mode_upper)
