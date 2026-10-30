#include "presentation_host.hpp"

#if defined(XENON_HAS_PRESENTATION_HOST)
#include <SDL3/SDL.h>

#if defined(XENON_HAS_VULKAN)
#include <SDL3/SDL_vulkan.h>
#include "xenon/gpu/vulkan/backend.hpp"
#endif
#if defined(XENON_HAS_D3D12)
#include "xenon/gpu/d3d12/backend.hpp"
#endif

namespace xenon::runtime_host {

namespace {

// Creates the window centred, sized in physical pixels: without high pixel
// density, Windows display scaling would bitmap-stretch the window and blur
// every presented frame, whatever size the swapchain is.
SDL_Window* create_window(const std::string& title, std::uint32_t width, std::uint32_t height,
                          SDL_WindowFlags flags) {
  auto* window = SDL_CreateWindow(title.c_str(), static_cast<int>(width),
                                  static_cast<int>(height),
                                  flags | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  if (window) SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
  return window;
}

}  // namespace

PresentationHost::~PresentationHost() { destroy(); }

void* PresentationHost::native_window() const noexcept {
#if defined(_WIN32)
  if (!window_) return nullptr;
  return SDL_GetPointerProperty(SDL_GetWindowProperties(window_),
                                SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#else
  return nullptr;
#endif
}

bool PresentationHost::create(std::uint32_t width, std::uint32_t height,
                              const std::string& title, xenon::gpu::Backend& backend,
                              std::string* error) {
  if (window_) {
    if (error) *error = "presentation window already created";
    return false;
  }

  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    if (error) *error = std::string("SDL_InitSubSystem(SDL_INIT_VIDEO) failed: ") + SDL_GetError();
    return false;
  }

  // A client area as large as the desktop (1920x1080 on a 1080p monitor)
  // can't fit beside the title bar and taskbar, so it starts maximized
  // instead of hanging off screen; the swapchain follows the real client
  // size through the resize events.
  SDL_WindowFlags size_flags = 0;
  SDL_Rect usable{};
  if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) &&
      (width > static_cast<std::uint32_t>(usable.w) ||
       height > static_cast<std::uint32_t>(usable.h))) {
    size_flags = SDL_WINDOW_MAXIMIZED;
  }

#if defined(XENON_HAS_VULKAN)
  if (auto* vulkan = dynamic_cast<xenon::gpu::vulkan::Backend*>(&backend)) {
    window_ = create_window(title, width, height, SDL_WINDOW_VULKAN | size_flags);
    if (!window_) {
      if (error) *error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
      SDL_QuitSubSystem(SDL_INIT_VIDEO);
      return false;
    }
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window_, vulkan->instance(), nullptr, &surface)) {
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
    window_ = create_window(title, width, height, size_flags);
    if (!window_) {
      if (error) *error = std::string("SDL_CreateWindow failed: ") + SDL_GetError();
      SDL_QuitSubSystem(SDL_INIT_VIDEO);
      return false;
    }
    void* hwnd = native_window();
    if (!hwnd) {
      if (error) *error = "Could not retrieve a native HWND from the SDL window";
      SDL_DestroyWindow(window_);
      window_ = nullptr;
      SDL_QuitSubSystem(SDL_INIT_VIDEO);
      return false;
    }
    xenon::gpu::PresentationConfig config{};
    config.width = width;
    config.height = height;
    if (!d3d12->configure_presentation(hwnd, config)) {
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

  const auto window_id = SDL_GetWindowID(window_);
  SDL_Event event{};
  while (SDL_PollEvent(&event)) {
    switch (event.type) {
      case SDL_EVENT_QUIT:
        out_events.close_requested = true;
        break;
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (event.window.windowID == window_id) out_events.close_requested = true;
        break;
      case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
        if (event.window.windowID != window_id) break;
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
      case SDL_EVENT_WINDOW_FOCUS_GAINED:
      case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (event.window.windowID != window_id) break;
        out_events.focus_changed = true;
        out_events.focused = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
        break;
      case SDL_EVENT_KEY_DOWN:
      case SDL_EVENT_KEY_UP:
        // Auto-repeat is not a new press; SDL scancodes are HID usages.
        if (event.key.windowID != window_id || event.key.repeat) break;
        out_events.input.push_back({PresentationInput::Kind::Key,
                                    static_cast<std::uint16_t>(event.key.scancode),
                                    event.type == SDL_EVENT_KEY_DOWN});
        break;
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (event.button.windowID != window_id) break;
        std::uint16_t button = 0;
        switch (event.button.button) {
          case SDL_BUTTON_LEFT: button = 1; break;
          case SDL_BUTTON_RIGHT: button = 2; break;
          case SDL_BUTTON_MIDDLE: button = 3; break;
          case SDL_BUTTON_X1: button = 4; break;
          case SDL_BUTTON_X2: button = 5; break;
          default: break;
        }
        if (button) {
          out_events.input.push_back({PresentationInput::Kind::MouseButton, button,
                                      event.type == SDL_EVENT_MOUSE_BUTTON_DOWN});
        }
        break;
      }
      case SDL_EVENT_MOUSE_MOTION:
        if (event.motion.windowID != window_id) break;
        out_events.input.push_back({PresentationInput::Kind::MouseMotion, 0, false,
                                    event.motion.xrel, event.motion.yrel});
        break;
      default:
        break;
    }
  }
}

#else

namespace xenon::runtime_host {

PresentationHost::~PresentationHost() = default;

void* PresentationHost::native_window() const noexcept { return nullptr; }

bool PresentationHost::create(std::uint32_t, std::uint32_t, const std::string&,
                              xenon::gpu::Backend&, std::string* error) {
  if (error) {
    *error =
        "Presentation is unavailable because this Xenon build has no SDL3 "
        "runtime-host window backend";
  }
  return false;
}

void PresentationHost::destroy() noexcept {
  window_ = nullptr;
  backend_ = nullptr;
}

void PresentationHost::pump_events(PresentationEvents& out_events) {
  out_events = {};
}

#endif  // XENON_HAS_PRESENTATION_HOST

}  // namespace xenon::runtime_host
