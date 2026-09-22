#include "presentation_host.hpp"

#include <SDL.h>

#if defined(XENON_HAS_VULKAN)
#include <SDL_vulkan.h>
#include "xenon/gpu/vulkan/backend.hpp"
#endif
#if defined(XENON_HAS_D3D12)
#define WIN32_LEAN_AND_MEAN
#include <SDL_syswm.h>
#include "xenon/gpu/d3d12/backend.hpp"
#endif

namespace xenon::runtime_host {

PresentationHost::~PresentationHost() { destroy(); }

bool PresentationHost::create(std::uint32_t width, std::uint32_t height,
                              const std::string& title, xenon::gpu::Backend& backend,
                              std::string* error) {
  if (window_) {
    if (error) *error = "presentation window already created";
    return false;
  }

  if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
    if (error) *error = std::string("SDL_InitSubSystem(SDL_INIT_VIDEO) failed: ") + SDL_GetError();
    return false;
  }

#if defined(XENON_HAS_VULKAN)
  if (auto* vulkan = dynamic_cast<xenon::gpu::vulkan::Backend*>(&backend)) {
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN;
    window_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              static_cast<int>(width), static_cast<int>(height), flags);
    if (!window_) {
      if (error) *error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
      SDL_QuitSubSystem(SDL_INIT_VIDEO);
      return false;
    }
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window_, vulkan->instance(), &surface)) {
      if (error) *error = std::string("SDL_Vulkan_CreateSurface failed: ") + SDL_GetError();
      SDL_DestroyWindow(window_);
      window_ = nullptr;
      SDL_QuitSubSystem(SDL_INIT_VIDEO);
      return false;
    }
    xenon::gpu::PresentationConfig config{};
    config.width = width;
    config.height = height;
    if (!vulkan->configure_presentation(surface, config, /*take_surface_ownership=*/true)) {
      if (error) *error = "Vulkan configure_presentation() failed";
      SDL_DestroyWindow(window_);
      window_ = nullptr;
      SDL_QuitSubSystem(SDL_INIT_VIDEO);
      return false;
    }
    backend_ = &backend;
    return true;
  }
#endif

#if defined(XENON_HAS_D3D12)
  if (auto* d3d12 = dynamic_cast<xenon::gpu::d3d12::Backend*>(&backend)) {
    Uint32 flags = SDL_WINDOW_RESIZABLE;
    window_ = SDL_CreateWindow(title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              static_cast<int>(width), static_cast<int>(height), flags);
    if (!window_) {
      if (error) *error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
      SDL_QuitSubSystem(SDL_INIT_VIDEO);
      return false;
    }
    SDL_SysWMinfo wm_info{};
    SDL_VERSION(&wm_info.version);
    if (!SDL_GetWindowWMInfo(window_, &wm_info) || wm_info.subsystem != SDL_SYSWM_WINDOWS) {
      if (error) *error = "Could not retrieve a native HWND from the SDL window";
      SDL_DestroyWindow(window_);
      window_ = nullptr;
      SDL_QuitSubSystem(SDL_INIT_VIDEO);
      return false;
    }
    xenon::gpu::PresentationConfig config{};
    config.width = width;
    config.height = height;
    if (!d3d12->configure_presentation(wm_info.info.win.window, config)) {
      if (error) *error = "D3D12 configure_presentation() failed";
      SDL_DestroyWindow(window_);
      window_ = nullptr;
      SDL_QuitSubSystem(SDL_INIT_VIDEO);
      return false;
    }
    backend_ = &backend;
    return true;
  }
#endif

  if (error) {
    *error =
        "No presentation path for this graphics backend (Null/headless backends must not "
        "create a window)";
  }
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
  return false;
}

void PresentationHost::destroy() noexcept {
  if (window_) {
    SDL_DestroyWindow(window_);
    window_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
  }
  backend_ = nullptr;
}

void PresentationHost::pump_events(PresentationEvents& out_events) {
  out_events = {};
  if (!window_) return;

  SDL_Event event{};
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
      case SDL_QUIT:
        out_events.close_requested = true;
        break;
      case SDL_WINDOWEVENT:
        if (event.window.windowID != SDL_GetWindowID(window_)) break;
        switch (event.window.event) {
          case SDL_WINDOWEVENT_CLOSE:
            out_events.close_requested = true;
            break;
          case SDL_WINDOWEVENT_RESIZED:
          case SDL_WINDOWEVENT_SIZE_CHANGED: {
            const auto width = static_cast<std::uint32_t>(event.window.data1);
            const auto height = static_cast<std::uint32_t>(event.window.data2);
            // A minimized window reports a zero-size surface; skip the
            // recreation attempt rather than handing the backend a
            // degenerate swapchain size (see resize_presentation() callers).
            if (width > 0 && height > 0 && backend_) {
              out_events.resized = true;
              out_events.width = width;
              out_events.height = height;
              static_cast<void>(backend_->resize_presentation(width, height));
            }
            break;
          }
          case SDL_WINDOWEVENT_FOCUS_GAINED:
            out_events.focus_changed = true;
            out_events.focused = true;
            break;
          case SDL_WINDOWEVENT_FOCUS_LOST:
            out_events.focus_changed = true;
            out_events.focused = false;
            break;
          default:
            break;
        }
        break;
      default:
        break;
    }
  }
}

}  // namespace xenon::runtime_host
