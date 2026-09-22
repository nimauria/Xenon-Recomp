#pragma once

// The game window belongs to XenonRuntime (this process), not XenonLauncher -
// see docs/runtime/RUNTIME_HOST.md. Reuses SDL2 (already an Xenon dependency for
// input/audio) rather than adding a second window/UI toolkit: this is a game
// host, not a second launcher UI. Owns no Xbox semantics - it only creates a
// native window/surface and hands it to the already-implemented Vulkan/D3D12
// presentation code (gpu::vulkan::Backend::configure_presentation(),
// gpu::d3d12::Backend::configure_presentation()).

#include <cstdint>
#include <string>

#include "xenon/gpu/backend.hpp"

struct SDL_Window;

namespace xenon::runtime_host {

struct PresentationEvents {
  bool close_requested{false};
  bool resized{false};
  std::uint32_t width{0};
  std::uint32_t height{0};
  bool focus_changed{false};
  bool focused{false};
};

class PresentationHost {
 public:
  ~PresentationHost();

  // Creates the window and wires it to `backend` (Vulkan or D3D12 - detected
  // via dynamic_cast; any other concrete xenon::gpu::Backend, notably
  // NullBackend, is rejected since headless/test sessions must not create a
  // window at all - callers should simply not call create() for those).
  [[nodiscard]] bool create(std::uint32_t width, std::uint32_t height,
                            const std::string& title, xenon::gpu::Backend& backend,
                            std::string* error);
  void destroy() noexcept;
  [[nodiscard]] bool created() const noexcept { return window_ != nullptr; }

  // Pumps the SDL event queue once, aggregating everything that happened
  // since the last call. Resize is applied directly to the backend here
  // (resize_presentation() is safe to call from the same thread that pumps
  // events); the caller still gets resized/width/height for logging/status.
  void pump_events(PresentationEvents& out_events);

 private:
  SDL_Window* window_{nullptr};
  xenon::gpu::Backend* backend_{nullptr};
};

}  // namespace xenon::runtime_host
