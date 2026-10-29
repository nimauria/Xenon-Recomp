#include "xenon/gpu/display_vblank.hpp"

#include <cmath>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi.h>
#include <wrl/client.h>
#endif

namespace xenon::gpu {

std::uint32_t host_refreshes_per_guest_vblank(double host_refresh_hz) noexcept {
  if (!(host_refresh_hz > 0.0) || !std::isfinite(host_refresh_hz)) return 0u;
  const double multiple = std::round(host_refresh_hz / kGuestVblankHz);
  if (multiple < 1.0 || multiple > 16.0) return 0u;
  return std::abs(host_refresh_hz / multiple - kGuestVblankHz) <= 1.0
             ? static_cast<std::uint32_t>(multiple)
             : 0u;
}

#if defined(_WIN32)
struct HostDisplayVblank::State {
  Microsoft::WRL::ComPtr<IDXGIOutput> output;
  double refresh_hz{};
};

HostDisplayVblank::HostDisplayVblank() : state_(std::make_unique<State>()) {}
HostDisplayVblank::~HostDisplayVblank() = default;

bool HostDisplayVblank::select(void* native_window) {
  state_->output.Reset();
  state_->refresh_hz = 0.0;
  const HMONITOR monitor =
      native_window ? MonitorFromWindow(static_cast<HWND>(native_window), MONITOR_DEFAULTTOPRIMARY)
                    : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (!monitor || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  for (UINT a = 0; !state_->output && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND;
       ++a) {
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
      DXGI_OUTPUT_DESC desc{};
      if (FAILED(output->GetDesc(&desc)) || desc.Monitor != monitor) continue;
      DEVMODEW mode{};
      mode.dmSize = sizeof(mode);
      if (EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode) &&
          mode.dmDisplayFrequency > 1u) {
        state_->refresh_hz = static_cast<double>(mode.dmDisplayFrequency);
      }
      state_->output = output;
      break;
    }
  }
  return available();
}

bool HostDisplayVblank::available() const noexcept { return state_->output != nullptr; }
double HostDisplayVblank::refresh_hz() const noexcept { return state_->refresh_hz; }

bool HostDisplayVblank::wait() {
  return state_->output && SUCCEEDED(state_->output->WaitForVBlank());
}
#else
// No display vblank source on this host yet: guest vblanks stay timer-paced.
struct HostDisplayVblank::State {};

HostDisplayVblank::HostDisplayVblank() : state_(std::make_unique<State>()) {}
HostDisplayVblank::~HostDisplayVblank() = default;
bool HostDisplayVblank::select(void*) { return false; }
bool HostDisplayVblank::available() const noexcept { return false; }
double HostDisplayVblank::refresh_hz() const noexcept { return 0.0; }
bool HostDisplayVblank::wait() { return false; }
#endif

}  // namespace xenon::gpu
