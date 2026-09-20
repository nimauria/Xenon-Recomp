#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>

namespace xenon::input {

// Frontend input is normalized before it reaches a UI surface. Host drivers
// remain responsible for translating native events into these neutral events.
enum class FrontendInputSource : std::uint8_t {
  Keyboard,
  Mouse,
  Touch,
  Touchpad,
  Pen,
  Gamepad,
  Wheel,
  FlightStick,
  MotionSensor,
  Voice,
  Accessibility,
};

enum class FrontendInputAction : std::uint8_t {
  Up,
  Down,
  Left,
  Right,
  Confirm,
  Cancel,
  Menu,
  QuickCenter,
  PageBack,
  PageForward,
  Search,
  Accept,
  Select,
  ScrollUp,
  ScrollDown,
};

struct FrontendInputEvent {
  FrontendInputSource source{FrontendInputSource::Keyboard};
  FrontendInputAction action{FrontendInputAction::Confirm};
  std::int32_t value{};
  std::uint64_t timestamp{};
  bool pressed{};
  bool repeated{};
};

struct FrontendInputBinding {
  FrontendInputSource source{FrontendInputSource::Keyboard};
  std::int32_t code{};
  FrontendInputAction action{FrontendInputAction::Confirm};
  bool allow_repeat{true};
};

// Shared action router for launcher, overlays and future accessibility
// frontends. It deliberately does not know about QML or window controls.
class FrontendInputRouter {
 public:
  void bind(const FrontendInputBinding& binding);
  void clear();
  void dispatch(FrontendInputSource source, std::int32_t code, bool pressed,
                bool repeated, std::int32_t value = 0,
                std::uint64_t timestamp = 0);
  void push(const FrontendInputEvent& event);
  [[nodiscard]] bool poll(FrontendInputEvent& event);
  [[nodiscard]] std::size_t pending() const noexcept { return events_.size(); }

 private:
  std::unordered_map<std::uint64_t, FrontendInputAction> bindings_{};
  std::deque<FrontendInputEvent> events_{};
};

}  // namespace xenon::input
