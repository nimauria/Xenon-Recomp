# Centralized third-party dependency resolution for Xenon.
#
# Release builds must not require users to install Xenia or SDL manually.
# Windows x64 uses the Xenon-owned FFmpeg/XMA bundle committed under
# third_party/xenon-ffmpeg. SDL2 is resolved from the host first and may be
# fetched from a pinned upstream revision for source builds.

include_guard(GLOBAL)
include(FetchContent)

option(XENON_FETCH_MISSING_DEPS
  "Fetch pinned open-source dependencies when they are not installed locally"
  ON)

set(XENON_SDL2_GIT_REPOSITORY "https://github.com/libsdl-org/SDL.git" CACHE STRING
  "SDL2 source repository used by Xenon's dependency bootstrap")
set(XENON_SDL2_GIT_TAG "b90ac95029d801c5abc59472ba8e2200dff31e1e" CACHE STRING
  "Pinned SDL2 revision used when Xenon fetches SDL2")

function(xenon_resolve_sdl2 out_target)
  if(TARGET SDL2::SDL2)
    set(${out_target} SDL2::SDL2 PARENT_SCOPE)
    return()
  elseif(TARGET SDL2::SDL2-static)
    set(${out_target} SDL2::SDL2-static PARENT_SCOPE)
    return()
  endif()

  find_package(SDL2 2.0.9 QUIET CONFIG)
  if(NOT TARGET SDL2::SDL2 AND NOT TARGET SDL2::SDL2-static)
    find_package(SDL2 2.0.9 QUIET)
  endif()

  if(TARGET SDL2::SDL2)
    set(${out_target} SDL2::SDL2 PARENT_SCOPE)
    return()
  elseif(TARGET SDL2::SDL2-static)
    set(${out_target} SDL2::SDL2-static PARENT_SCOPE)
    return()
  elseif(SDL2_FOUND AND SDL2_LIBRARIES)
    if(NOT TARGET xenon_sdl2_external)
      add_library(xenon_sdl2_external INTERFACE)
      target_include_directories(xenon_sdl2_external INTERFACE ${SDL2_INCLUDE_DIRS})
      target_link_libraries(xenon_sdl2_external INTERFACE ${SDL2_LIBRARIES})
    endif()
    set(${out_target} xenon_sdl2_external PARENT_SCOPE)
    return()
  endif()

  if(XENON_FETCH_MISSING_DEPS)
    message(STATUS "SDL2 was not found locally; fetching Xenon's pinned SDL2 revision ${XENON_SDL2_GIT_TAG}")
    # Static SDL keeps the packaged runtime self-contained and avoids requiring
    # end users to install or copy SDL2.dll separately.
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
    FetchContent_Declare(xenon_sdl2
      GIT_REPOSITORY "${XENON_SDL2_GIT_REPOSITORY}"
      GIT_TAG "${XENON_SDL2_GIT_TAG}"
      GIT_PROGRESS TRUE)
    FetchContent_MakeAvailable(xenon_sdl2)

    if(TARGET SDL2::SDL2-static)
      set(${out_target} SDL2::SDL2-static PARENT_SCOPE)
      return()
    elseif(TARGET SDL2::SDL2)
      set(${out_target} SDL2::SDL2 PARENT_SCOPE)
      return()
    endif()
  endif()

  # Final manual search supports SDK-style installations that don't ship a
  # CMake package file.
  find_path(_xenon_sdl2_include SDL.h PATH_SUFFIXES SDL2 NO_CACHE)
  find_library(_xenon_sdl2_library NAMES SDL2 SDL2-static NO_CACHE)
  if(_xenon_sdl2_include AND _xenon_sdl2_library)
    if(NOT TARGET xenon_sdl2_external)
      add_library(xenon_sdl2_external INTERFACE)
      target_include_directories(xenon_sdl2_external INTERFACE "${_xenon_sdl2_include}")
      target_link_libraries(xenon_sdl2_external INTERFACE "${_xenon_sdl2_library}")
    endif()
    set(${out_target} xenon_sdl2_external PARENT_SCOPE)
  else()
    set(${out_target} "" PARENT_SCOPE)
  endif()
endfunction()

function(xenon_resolve_audio_ffmpeg out_target out_include out_bundled)
  set(_target "")
  set(_include "")
  set(_bundled FALSE)
  set(_root "${XENON_AUDIO_FFMPEG_ROOT}")

  # A normal Windows x64 checkout is self-contained: prefer Xenon's vetted
  # XMAFRAMES build unless a developer explicitly supplies another root.
  if(NOT _root AND WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_candidate "${PROJECT_SOURCE_DIR}/third_party/xenon-ffmpeg/windows-x64")
    if(EXISTS "${_candidate}/include/libavcodec/avcodec.h" AND
       EXISTS "${_candidate}/include/libavcodec/codec_id.h" AND
       EXISTS "${_candidate}/include/libavutil/avutil.h" AND
       EXISTS "${_candidate}/lib/libavcodec.lib" AND
       EXISTS "${_candidate}/lib/libavutil.lib")
      set(_root "${_candidate}")
      set(_bundled TRUE)
      message(STATUS "Using bundled Xenon FFmpeg/XMA dependency: ${_root}")
    endif()
  endif()

  if(_bundled)
    set(_bundle_manifest "${_root}/XenonFFmpegBundle.cmake")
    if(NOT EXISTS "${_bundle_manifest}")
      message(FATAL_ERROR "Bundled Xenon FFmpeg is missing its identity manifest: ${_bundle_manifest}")
    endif()
    include("${_bundle_manifest}")
    file(SHA256 "${_root}/lib/libavcodec.lib" _bundle_avcodec_sha256)
    file(SHA256 "${_root}/lib/libavutil.lib" _bundle_avutil_sha256)
    file(SHA256 "${_root}/include/libavcodec/codec_id.h" _bundle_codec_id_sha256)
    if(NOT _bundle_avcodec_sha256 STREQUAL XENON_FFMPEG_BUNDLE_AVCODEC_SHA256 OR
       NOT _bundle_avutil_sha256 STREQUAL XENON_FFMPEG_BUNDLE_AVUTIL_SHA256 OR
       NOT _bundle_codec_id_sha256 STREQUAL XENON_FFMPEG_BUNDLE_CODEC_ID_SHA256)
      message(FATAL_ERROR
        "Bundled Xenon FFmpeg/XMA dependency failed its SHA-256 identity check. Rebuild it with tools/rebuild_xenon_ffmpeg.ps1 or restore the committed bundle.")
    endif()

    if(NOT TARGET xenon_ffmpeg_avutil)
      add_library(xenon_ffmpeg_avutil STATIC IMPORTED GLOBAL)
      set_target_properties(xenon_ffmpeg_avutil PROPERTIES
        IMPORTED_LOCATION "${_root}/lib/libavutil.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${_root}/include")
    endif()
    if(NOT TARGET xenon_ffmpeg_avcodec)
      add_library(xenon_ffmpeg_avcodec STATIC IMPORTED GLOBAL)
      set_target_properties(xenon_ffmpeg_avcodec PROPERTIES
        IMPORTED_LOCATION "${_root}/lib/libavcodec.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${_root}/include")
      target_link_libraries(xenon_ffmpeg_avcodec INTERFACE xenon_ffmpeg_avutil bcrypt)
    endif()
    if(NOT TARGET xenon_audio_ffmpeg_bundled)
      add_library(xenon_audio_ffmpeg_bundled INTERFACE)
      target_link_libraries(xenon_audio_ffmpeg_bundled INTERFACE xenon_ffmpeg_avcodec)
    endif()
    set(_target xenon_audio_ffmpeg_bundled)
    set(_include "${_root}/include")
  elseif(_root)
    find_path(_ffmpeg_include libavcodec/avcodec.h
      HINTS "${_root}" "${_root}/include" NO_DEFAULT_PATH NO_CACHE)
    find_library(_ffmpeg_avcodec NAMES avcodec libavcodec
      HINTS "${_root}" "${_root}/lib" NO_DEFAULT_PATH NO_CACHE)
    find_library(_ffmpeg_avutil NAMES avutil libavutil
      HINTS "${_root}" "${_root}/lib" NO_DEFAULT_PATH NO_CACHE)
    if(_ffmpeg_include AND _ffmpeg_avcodec AND _ffmpeg_avutil)
      if(NOT TARGET xenon_audio_ffmpeg_external)
        add_library(xenon_audio_ffmpeg_external INTERFACE)
        target_include_directories(xenon_audio_ffmpeg_external INTERFACE "${_ffmpeg_include}")
        target_link_libraries(xenon_audio_ffmpeg_external INTERFACE
          "${_ffmpeg_avcodec}" "${_ffmpeg_avutil}")
        if(WIN32)
          target_link_libraries(xenon_audio_ffmpeg_external INTERFACE bcrypt)
        endif()
      endif()
      set(_target xenon_audio_ffmpeg_external)
      set(_include "${_ffmpeg_include}")
    endif()
  else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
      pkg_check_modules(XENON_AUDIO_FFMPEG QUIET IMPORTED_TARGET libavcodec libavutil)
    endif()
    if(TARGET PkgConfig::XENON_AUDIO_FFMPEG)
      set(_target PkgConfig::XENON_AUDIO_FFMPEG)
      get_target_property(_include PkgConfig::XENON_AUDIO_FFMPEG INTERFACE_INCLUDE_DIRECTORIES)
    else()
      find_path(_ffmpeg_include libavcodec/avcodec.h NO_CACHE)
      find_library(_ffmpeg_avcodec NAMES avcodec libavcodec NO_CACHE)
      find_library(_ffmpeg_avutil NAMES avutil libavutil NO_CACHE)
      if(_ffmpeg_include AND _ffmpeg_avcodec AND _ffmpeg_avutil)
        if(NOT TARGET xenon_audio_ffmpeg_external)
          add_library(xenon_audio_ffmpeg_external INTERFACE)
          target_include_directories(xenon_audio_ffmpeg_external INTERFACE "${_ffmpeg_include}")
          target_link_libraries(xenon_audio_ffmpeg_external INTERFACE
            "${_ffmpeg_avcodec}" "${_ffmpeg_avutil}")
          if(WIN32)
            target_link_libraries(xenon_audio_ffmpeg_external INTERFACE bcrypt)
          endif()
        endif()
        set(_target xenon_audio_ffmpeg_external)
        set(_include "${_ffmpeg_include}")
      endif()
    endif()
  endif()

  set(${out_target} "${_target}" PARENT_SCOPE)
  set(${out_include} "${_include}" PARENT_SCOPE)
  set(${out_bundled} "${_bundled}" PARENT_SCOPE)
endfunction()
