#include "xenon/audio/backend.hpp"

#include <SDL.h>

#include <algorithm>
#include <mutex>
#include <utility>

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
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
      if (error) *error = SDL_GetError();
      return false;
    }
    sdl_initialized_ = true;

    SDL_AudioSpec desired{};
    desired.freq = static_cast<int>(config.sample_rate);
    desired.format = AUDIO_F32SYS;
    desired.channels = static_cast<Uint8>(config.channels);
    desired.samples = static_cast<Uint16>(std::min<std::uint32_t>(
        config.callback_frames, 0xFFFFu));
    desired.callback = &SdlAudioBackend::callback_thunk;
    desired.userdata = this;

    {
      std::lock_guard lock(callback_mutex_);
      callback_ = std::move(callback);
    }
    device_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained_, 0);
    if (!device_) {
      if (error) *error = SDL_GetError();
      close();
      return false;
    }
    if (obtained_.format != AUDIO_F32SYS || obtained_.channels != 2 ||
        obtained_.freq != desired.freq) {
      if (error) *error = "SDL audio device did not accept the required float32 stereo format";
      close();
      return false;
    }
    SDL_PauseAudioDevice(device_, 0);
    return true;
  }

  void close() noexcept override {
    if (device_) {
      SDL_PauseAudioDevice(device_, 1);
      SDL_CloseAudioDevice(device_);
      device_ = 0;
    }
    {
      std::lock_guard lock(callback_mutex_);
      callback_ = {};
    }
    obtained_ = {};
    if (sdl_initialized_) {
      SDL_QuitSubSystem(SDL_INIT_AUDIO);
      sdl_initialized_ = false;
    }
  }

  bool is_open() const noexcept override { return device_ != 0; }

 private:
  static void callback_thunk(void* userdata, Uint8* stream, int len) {
    auto* self = static_cast<SdlAudioBackend*>(userdata);
    if (!self || !stream || len <= 0) return;
    auto* output = reinterpret_cast<float*>(stream);
    const auto count = static_cast<std::size_t>(len) / sizeof(float);
    std::fill_n(output, count, 0.0f);

    HostRenderCallback callback;
    {
      std::lock_guard lock(self->callback_mutex_);
      callback = self->callback_;
    }
    if (callback) callback(std::span<float>(output, count));
  }

  SDL_AudioDeviceID device_{};
  SDL_AudioSpec obtained_{};
  bool sdl_initialized_{};
  std::mutex callback_mutex_{};
  HostRenderCallback callback_{};
};

}  // namespace

std::unique_ptr<AudioBackend> create_sdl_audio_backend() {
  return std::make_unique<SdlAudioBackend>();
}

}  // namespace xenon::audio
