#include "xenon/input/scripted_driver.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <span>
#include <utility>

namespace xenon::input {
namespace {

using std::chrono::milliseconds;

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
  return s;
}

std::string upper(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return out;
}

std::vector<std::string_view> words(std::string_view s) {
  std::vector<std::string_view> out;
  while (true) {
    s = trim(s);
    if (s.empty()) break;
    std::size_t end = 0;
    while (end < s.size() && !std::isspace(static_cast<unsigned char>(s[end]))) ++end;
    out.push_back(s.substr(0, end));
    s.remove_prefix(end);
  }
  return out;
}

std::optional<double> number(std::string_view s) {
  if (s.empty()) return std::nullopt;
  double value = 0.0;
  const auto* first = s.data();
  if (*first == '+') ++first;
  const auto [ptr, ec] = std::from_chars(first, s.data() + s.size(), value);
  if (ec != std::errc{} || ptr != s.data() + s.size() || !std::isfinite(value)) return std::nullopt;
  return value;
}

// "1500ms", "2.5s" or "2s" -> milliseconds (non-negative).
std::optional<milliseconds> duration(std::string_view s) {
  double scale = 0.0;
  if (s.size() > 2 && s.substr(s.size() - 2) == "ms") {
    scale = 1.0;
    s.remove_suffix(2);
  } else if (s.size() > 1 && s.back() == 's') {
    scale = 1000.0;
    s.remove_suffix(1);
  } else {
    return std::nullopt;
  }
  const auto value = number(s);
  if (!value || *value < 0.0 || *value * scale > 1e12) return std::nullopt;
  return milliseconds(static_cast<std::int64_t>(std::llround(*value * scale)));
}

std::optional<std::uint16_t> button(std::string_view name) {
  const auto n = upper(name);
  if (n == "A") return A;
  if (n == "B") return B;
  if (n == "X") return X;
  if (n == "Y") return Y;
  if (n == "START") return Start;
  if (n == "BACK") return Back;
  if (n == "LB") return LeftShoulder;
  if (n == "RB") return RightShoulder;
  if (n == "LS") return LeftThumb;
  if (n == "RS") return RightThumb;
  if (n == "UP") return DpadUp;
  if (n == "DOWN") return DpadDown;
  if (n == "LEFT") return DpadLeft;
  if (n == "RIGHT") return DpadRight;
  if (n == "GUIDE") return Guide;
  return std::nullopt;
}

std::optional<std::int16_t> axis(std::string_view s) {
  const auto v = number(s);
  if (!v || *v < -1.0 || *v > 1.0) return std::nullopt;
  return static_cast<std::int16_t>(std::lround(*v >= 0.0 ? *v * 32767.0 : *v * 32768.0));
}

bool fail(std::string& error, std::uint32_t line, std::string_view why) {
  error = "input script line " + std::to_string(line) + ": " + std::string(why);
  return false;
}

// Parses "<action> [args] [for <duration>]" from `w` into `event`.
bool parse_action(std::span<const std::string_view> w, std::uint32_t line, ScriptedInputEvent& event,
                  std::string& error) {
  if (w.empty()) return fail(error, line, "missing action");
  event.duration = milliseconds(100);
  if (w.size() >= 2 && w[w.size() - 2] == "for") {
    const auto d = duration(w.back());
    if (!d || d->count() == 0) return fail(error, line, "bad duration '" + std::string(w.back()) + "'");
    event.duration = *d;
    w = w.first(w.size() - 2);
  }
  const auto action = upper(w[0]);
  if (action == "PRESS") {
    if (w.size() != 2) return fail(error, line, "press takes one button list (A+B)");
    event.kind = ScriptedInputEvent::Kind::Buttons;
    std::string_view list = w[1];
    while (!list.empty()) {
      const auto plus = list.find('+');
      const auto name = list.substr(0, plus);
      const auto b = button(name);
      if (!b) return fail(error, line, "unknown button '" + std::string(name) + "'");
      event.buttons = static_cast<std::uint16_t>(event.buttons | *b);
      if (plus == std::string_view::npos) break;
      list.remove_prefix(plus + 1);
    }
    return true;
  }
  if (action == "STICK") {
    if (w.size() != 4) return fail(error, line, "stick takes <left|right> <x> <y>");
    const auto side = upper(w[1]);
    if (side != "LEFT" && side != "RIGHT") return fail(error, line, "stick side must be left or right");
    const auto x = axis(w[2]);
    const auto y = axis(w[3]);
    if (!x || !y) return fail(error, line, "stick values must be in [-1, 1]");
    event.kind = side == "LEFT" ? ScriptedInputEvent::Kind::LeftStick : ScriptedInputEvent::Kind::RightStick;
    event.x = *x;
    event.y = *y;
    return true;
  }
  if (action == "TRIGGER") {
    if (w.size() != 3) return fail(error, line, "trigger takes <left|right> <value>");
    const auto side = upper(w[1]);
    if (side != "LEFT" && side != "RIGHT") return fail(error, line, "trigger side must be left or right");
    const auto v = number(w[2]);
    if (!v || *v < 0.0 || *v > 1.0) return fail(error, line, "trigger value must be in [0, 1]");
    event.kind = side == "LEFT" ? ScriptedInputEvent::Kind::LeftTrigger : ScriptedInputEvent::Kind::RightTrigger;
    event.trigger = static_cast<std::uint8_t>(std::lround(*v * 255.0));
    return true;
  }
  return fail(error, line, "unknown action '" + std::string(w[0]) + "'");
}

}  // namespace

milliseconds ScriptedInput::end() const noexcept {
  milliseconds end{};
  for (const auto& e : events) end = std::max(end, e.start + e.duration);
  return end;
}

std::optional<ScriptedInput> parse_input_script(std::string_view text, std::string& error) {
  ScriptedInput script;
  milliseconds previous{};
  std::uint32_t line_number = 0;
  while (!text.empty()) {
    const auto newline = text.find('\n');
    std::string_view line = text.substr(0, newline);
    text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
    ++line_number;
    if (const auto hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
    line = trim(line);
    if (line.empty()) continue;

    const auto w = words(line);
    std::string_view start_text = w[0];
    const bool relative = start_text.front() == '+';
    if (relative) start_text.remove_prefix(1);
    const auto at = duration(start_text);
    if (!at) {
      fail(error, line_number, "bad start time '" + std::string(w[0]) + "' (use 12s, 1500ms or +2s)");
      return std::nullopt;
    }
    const milliseconds start = relative ? previous + *at : *at;
    previous = start;
    if (w.size() < 2) {
      fail(error, line_number, "missing action");
      return std::nullopt;
    }

    if (upper(w[1]) == "LOG") {
      ScriptedInputEvent event;
      event.kind = ScriptedInputEvent::Kind::Log;
      event.start = start;
      event.line = line_number;
      const auto pos = line.find(w[1]) + w[1].size();
      event.text = std::string(trim(line.substr(pos)));
      script.events.push_back(std::move(event));
      continue;
    }

    std::uint32_t count = 1;
    milliseconds every{};
    std::span<const std::string_view> action(w.data() + 1, w.size() - 1);
    if (upper(w[1]) == "REPEAT") {
      if (w.size() < 6 || w[3] != "every") {
        fail(error, line_number, "repeat takes <count> every <interval> <action>");
        return std::nullopt;
      }
      const auto n = number(w[2]);
      const auto interval = duration(w[4]);
      if (!n || *n < 1.0 || *n > 100000.0 || std::floor(*n) != *n) {
        fail(error, line_number, "repeat count must be a whole number from 1 to 100000");
        return std::nullopt;
      }
      if (!interval || interval->count() == 0) {
        fail(error, line_number, "bad repeat interval '" + std::string(w[4]) + "'");
        return std::nullopt;
      }
      count = static_cast<std::uint32_t>(*n);
      every = *interval;
      action = action.subspan(4);
    }
    ScriptedInputEvent event;
    event.line = line_number;
    event.text = std::string(line);
    if (!parse_action(action, line_number, event, error)) return std::nullopt;
    for (std::uint32_t i = 0; i < count; ++i) {
      event.start = start + every * i;
      script.events.push_back(event);
    }
  }
  std::stable_sort(script.events.begin(), script.events.end(),
                   [](const auto& a, const auto& b) { return a.start < b.start; });
  return script;
}

GamepadState scripted_state_at(const ScriptedInput& script, milliseconds at) noexcept {
  GamepadState state{};
  for (const auto& e : script.events) {
    if (e.start > at) break;  // ordered by start
    if (at >= e.start + e.duration) continue;
    switch (e.kind) {
      case ScriptedInputEvent::Kind::Buttons:
        state.buttons = static_cast<std::uint16_t>(state.buttons | e.buttons);
        break;
      case ScriptedInputEvent::Kind::LeftStick:
        state.thumb_lx = e.x;
        state.thumb_ly = e.y;
        break;
      case ScriptedInputEvent::Kind::RightStick:
        state.thumb_rx = e.x;
        state.thumb_ry = e.y;
        break;
      case ScriptedInputEvent::Kind::LeftTrigger: state.left_trigger = e.trigger; break;
      case ScriptedInputEvent::Kind::RightTrigger: state.right_trigger = e.trigger; break;
      case ScriptedInputEvent::Kind::Log: break;
    }
  }
  return state;
}

ScriptedInputDriver::ScriptedInputDriver(ScriptedInput script, Clock clock, Reporter reporter)
    : script_(std::move(script)),
      clock_(clock ? std::move(clock) : Clock([] { return std::chrono::steady_clock::now(); })),
      reporter_(std::move(reporter)) {}

void ScriptedInputDriver::enumerate_devices(std::vector<DriverDeviceInfo>& out_devices) {
  out_devices.clear();
  DriverDeviceInfo info;
  info.native_id = kDevice;
  info.persistent_key = std::string(kPersistentKey);
  info.name = "Scripted controller";
  info.family = ControllerFamily::Virtual;
  info.connection = ConnectionType::Virtual;
  out_devices.push_back(std::move(info));
}

Result ScriptedInputDriver::get_state(NativeDeviceId device, GamepadState& out_state) {
  if (device != kDevice) return Result::DeviceNotConnected;
  const auto now = clock_();
  std::vector<std::pair<const ScriptedInputEvent*, milliseconds>> reached;
  {
    std::scoped_lock lock(mutex_);
    if (!zero_) zero_ = now;
    const auto at = std::chrono::duration_cast<milliseconds>(now - *zero_);
    out_state = scripted_state_at(script_, at);
    while (reached_ < script_.events.size() && script_.events[reached_].start <= at) {
      reached.emplace_back(&script_.events[reached_], at);
      ++reached_;
    }
  }
  if (reporter_) {
    for (const auto& [event, at] : reached) reporter_(*event, at);
  }
  return Result::Success;
}

Result ScriptedInputDriver::get_capabilities(NativeDeviceId device, Capabilities& out_caps) {
  if (device != kDevice) return Result::DeviceNotConnected;
  out_caps = {};
  // A standard gamepad reports every control it has as present.
  out_caps.gamepad.buttons = 0xF3FFu;
  out_caps.gamepad.left_trigger = 0xFFu;
  out_caps.gamepad.right_trigger = 0xFFu;
  out_caps.gamepad.thumb_lx = out_caps.gamepad.thumb_ly = static_cast<std::int16_t>(0xFFC0);
  out_caps.gamepad.thumb_rx = out_caps.gamepad.thumb_ry = static_cast<std::int16_t>(0xFFC0);
  return Result::Success;
}

Result ScriptedInputDriver::set_vibration(NativeDeviceId device, const Vibration&) {
  // No motors: report it as a controller without vibration does.
  return device == kDevice ? Result::Unsupported : Result::DeviceNotConnected;
}

Result ScriptedInputDriver::get_keystroke(NativeDeviceId device, Keystroke&) {
  return device == kDevice ? Result::Empty : Result::DeviceNotConnected;
}

std::optional<milliseconds> ScriptedInputDriver::elapsed() const {
  std::scoped_lock lock(mutex_);
  if (!zero_) return std::nullopt;
  return std::chrono::duration_cast<milliseconds>(clock_() - *zero_);
}

std::size_t ScriptedInputDriver::events_reached() const {
  std::scoped_lock lock(mutex_);
  return reached_;
}

}  // namespace xenon::input
