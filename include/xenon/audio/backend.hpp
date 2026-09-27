#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace xenon::audio {

struct AudioBackendConfig {
  std::uint32_t sample_rate{48000};
  std::uint32_t channels{2};
  std::uint32_t callback_frames{256};
};

using HostRenderCallback = std::function<void(std::span<float> interleaved)>;

class AudioBackend {
 public:
  virtual ~AudioBackend() = default;
  [[nodiscard]] virtual bool open(const AudioBackendConfig& config,
                                  HostRenderCallback callback,
                                  std::string* error = nullptr) = 0;
  virtual void close() noexcept = 0;
  [[nodiscard]] virtual bool is_open() const noexcept = 0;
};

// Production desktop backend. SDL2 provides the same implementation on
// Windows and Linux, avoiding guest-visible backend differences.
[[nodiscard]] std::unique_ptr<AudioBackend> create_sdl_audio_backend();

}  // namespace xenon::audio
