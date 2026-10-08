# -----------------------------------------------------------------------------
# Audio: Xbox render-driver/XMA semantics -> bounded Xenon mixer -> SDL2 host.
# FFmpeg supplies the legally redistributable XMAFRAMES decoder API; Xenon does
# not vendor an XMA codec or substitute a silent/null production backend.
if(XENON_ENABLE_AUDIO)
  xenon_resolve_sdl2(XENON_AUDIO_SDL_TARGET)
  xenon_resolve_audio_ffmpeg(
    XENON_AUDIO_FFMPEG_TARGET
    XENON_AUDIO_AVCODEC_INCLUDE_DIR
    XENON_AUDIO_USING_BUNDLED_FFMPEG)

  if(NOT XENON_AUDIO_SDL_TARGET)
    xenon_dependency_help("sdl2")
    message(FATAL_ERROR
      "Audio V1 requires SDL2. Xenon could not resolve or bootstrap the pinned dependency. "
      "Use XENON_DEPENDENCY_MODE=SYSTEM for a host package, set XENON_SDL2_ROOT, "
      "or provision the managed dependency with the command above.")
  endif()
  if(NOT XENON_AUDIO_FFMPEG_TARGET)
    xenon_dependency_help("xenia-ffmpeg")
    message(FATAL_ERROR
      "Audio V1 requires libavcodec/libavutil with AV_CODEC_ID_XMAFRAMES. "
      "Use Xenon's managed dependency bootstrap, the committed Windows x64 developer bundle, "
      "or set XENON_AUDIO_FFMPEG_ROOT to a compatible xenia-project FFmpeg build.")
  endif()

  include(CheckCXXSourceCompiles)
  # Header/API capability probes. Static-library try-compiles avoid attempting
  # to execute target binaries and work with prebuilt Windows archives.
  set(_xenon_audio_saved_try_compile_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
  set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
  set(CMAKE_REQUIRED_INCLUDES "${XENON_AUDIO_AVCODEC_INCLUDE_DIR}")
  check_cxx_source_compiles([=[
    extern "C" {
      #include <libavcodec/avcodec.h>
    }
    int main() { auto id = AV_CODEC_ID_XMAFRAMES; return static_cast<int>(id == AV_CODEC_ID_NONE); }
  ]=] XENON_AUDIO_FFMPEG_HAS_XMAFRAMES)
  if(NOT XENON_AUDIO_FFMPEG_HAS_XMAFRAMES)
    message(FATAL_ERROR
      "The selected FFmpeg does not expose AV_CODEC_ID_XMAFRAMES. Mainline FFmpeg is not a substitute for Xenon's raw XMA hardware-frame path. Use the managed dependency bootstrap, the vetted Windows x64 developer bundle, or XENON_AUDIO_FFMPEG_ROOT.")
  endif()

  check_cxx_source_compiles([=[
    extern "C" {
      #include <libavcodec/avcodec.h>
      #include <libavutil/channel_layout.h>
    }
    int main() { AVCodecContext* c = nullptr; av_channel_layout_default(&c->ch_layout, 2); return 0; }
  ]=] XENON_AUDIO_FFMPEG_HAS_CH_LAYOUT)
  if(_xenon_audio_saved_try_compile_target_type)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_xenon_audio_saved_try_compile_target_type}")
  else()
    unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
  endif()
  unset(CMAKE_REQUIRED_INCLUDES)
  unset(_xenon_audio_saved_try_compile_target_type)

  add_library(xenon_audio STATIC
    backend_sdl.cpp
    mixer.cpp
    system.cpp
    xma.cpp
  )
  target_include_directories(xenon_audio PUBLIC "${XENON_SOURCE_DIR}/include")
  target_link_libraries(xenon_audio
    PUBLIC xenon_memory
    PRIVATE ${XENON_AUDIO_SDL_TARGET} ${XENON_AUDIO_FFMPEG_TARGET})
  target_compile_features(xenon_audio PUBLIC cxx_std_20)
  if(XENON_AUDIO_FFMPEG_HAS_CH_LAYOUT)
    target_compile_definitions(xenon_audio PRIVATE XENON_AUDIO_FFMPEG_HAS_CH_LAYOUT=1)
  endif()
  if(XENON_AUDIO_USING_BUNDLED_FFMPEG)
    target_compile_definitions(xenon_audio PRIVATE XENON_AUDIO_BUNDLED_FFMPEG=1)
  endif()
  xenon_enable_warnings(xenon_audio)
  add_library(Xenon::Audio ALIAS xenon_audio)

  target_sources(xenon_core PRIVATE exports.cpp)
  target_compile_definitions(xenon_core PUBLIC XENON_HAS_AUDIO=1)
endif()
