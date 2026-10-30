#include "xenon/audio/backend.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace xenon::audio {
namespace {

class SdlAudioBackend final : public AudioBackend {
 public:
  ~SdlAudioBackend() override { close(); }

  bool open(const AudioBackendConfig& config, HostRenderCallback callback,
            std::string* error) override {
    close();
    if (!callback || !config.sample_rate || config.channels != 2 ||
        !config.callback_frames) {
      if (error) *error = "invalid SDL audio configuration";
      return false;
    }
    // The device buffer matches the mixer's render quantum, as SDL2's
    // `samples` did, so callbacks keep the same cadence.
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES,
                std::to_string(config.callback_frames).c_str());
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      if (error) *error = SDL_GetError();
      return false;
    }
    sdl_initialized_ = true;

    {
      std::lock_guard lock(callback_mutex_);
      callback_ = std::move(callback);
      chunk_.assign(static_cast<std::size_t>(config.callback_frames) * config.channels, 0.0f);
    }
    // The stream converts float32 stereo at the mixer's rate to whatever the
    // device runs at.
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = static_cast<int>(config.channels);
    spec.freq = static_cast<int>(config.sample_rate);
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                        &SdlAudioBackend::callback_thunk, this);
    if (!stream_) {
      if (error) *error = SDL_GetError();
      close();
      return false;
    }
    if (!SDL_ResumeAudioStreamDevice(stream_)) {
      if (error) *error = SDL_GetError();
      close();
      return false;
    }
    return true;
  }

  void close() noexcept override {
    if (stream_) {
      // Destroying the stream closes its device and stops the callback.
      SDL_DestroyAudioStream(stream_);
      stream_ = nullptr;
    }
    {
      std::lock_guard lock(callback_mutex_);
      callback_ = {};
      chunk_.clear();
    }
    if (sdl_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
      sdl_initialized_ = false;
    }
  }

  bool is_open() const noexcept override { return stream_ != nullptr; }

 private:
  // SDL asks for at least `additional_amount` more bytes. Render whole mixer
  // quanta until that is covered, so the mixer is always called with the
  // frame count it was configured for.
  static void SDLCALL callback_thunk(void* userdata, SDL_AudioStream* stream,
                                     int additional_amount, int /*total_amount*/) {
    auto* self = static_cast<SdlAudioBackend*>(userdata);
    if (!self || !stream || additional_amount <= 0) return;
    std::lock_guard lock(self->callback_mutex_);
    if (!self->callback_ || self->chunk_.empty()) return;
    const auto chunk_bytes = static_cast<int>(self->chunk_.size() * sizeof(float));
    for (int queued = 0; queued < additional_amount; queued += chunk_bytes) {
      std::fill(self->chunk_.begin(), self->chunk_.end(), 0.0f);
      self->callback_(std::span<float>(self->chunk_));
      if (!SDL_PutAudioStreamData(stream, self->chunk_.data(), chunk_bytes)) return;
    }
  }

  SDL_AudioStream* stream_{};
  bool sdl_initialized_{};
  std::mutex callback_mutex_{};
  HostRenderCallback callback_{};
  std::vector<float> chunk_{};
};

}  // namespace

std::unique_ptr<AudioBackend> create_sdl_audio_backend() {
  return std::make_unique<SdlAudioBackend>();
}

}  // namespace xenon::audio
