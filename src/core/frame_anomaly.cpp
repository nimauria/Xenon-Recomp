#include "xenon/core/frame_anomaly.hpp"

#include <cmath>
#include <cstdlib>

namespace xenon::core {

FrameColorStats frame_color_stats(std::span<const std::byte> rgba8, std::uint32_t width,
                                  std::uint32_t height, std::uint32_t row_pitch) {
  FrameColorStats stats{};
  if (width == 0u || height == 0u || row_pitch < width * 4u ||
      rgba8.size() < static_cast<std::size_t>(row_pitch) * (height - 1u) + width * 4u)
    return stats;
  double r = 0, g = 0, b = 0, lum = 0;
  std::uint64_t dark = 0, bright = 0, green = 0;
  for (std::uint32_t y = 0; y < height; ++y) {
    const auto* row = rgba8.data() + static_cast<std::size_t>(y) * row_pitch;
    for (std::uint32_t x = 0; x < width; ++x) {
      const int pr = std::to_integer<int>(row[x * 4u + 0u]);
      const int pg = std::to_integer<int>(row[x * 4u + 1u]);
      const int pb = std::to_integer<int>(row[x * 4u + 2u]);
      const double l = 0.299 * pr + 0.587 * pg + 0.114 * pb;
      r += pr;
      g += pg;
      b += pb;
      lum += l;
      dark += l < 16.0;
      bright += l > 235.0;
      green += pg > pr + 40 && pg > pb + 40;
    }
  }
  const double count = static_cast<double>(width) * height;
  stats.mean_r = r / count;
  stats.mean_g = g / count;
  stats.mean_b = b / count;
  stats.luminance = lum / count;
  stats.dark_fraction = static_cast<double>(dark) / count;
  stats.bright_fraction = static_cast<double>(bright) / count;
  stats.green_fraction = static_cast<double>(green) / count;
  stats.valid = true;
  return stats;
}

std::string_view classify_frame_change(const FrameColorStats& previous,
                                       const FrameColorStats& current) {
  if (!previous.valid || !current.valid) return {};
  if (current.dark_fraction > 0.97 && previous.dark_fraction < 0.5) return "black";
  if (current.bright_fraction > 0.85 && previous.bright_fraction < 0.5) return "white";
  if (current.green_fraction > 0.6 && previous.green_fraction < 0.3) return "green";
  if (std::fabs(current.luminance - previous.luminance) > 60.0) return "brightness jump";
  return {};
}

AnomalyCaptureGate::Options AnomalyCaptureGate::options_from_environment() {
  Options options{};
  const auto number = [](const char* name, std::uint64_t fallback) {
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)  // read once at session start
#endif
    const char* value = std::getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (!value || !*value) return fallback;
    char* end = nullptr;
    const auto parsed = std::strtoull(value, &end, 10);
    return end && *end == '\0' ? static_cast<std::uint64_t>(parsed) : fallback;
  };
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
  const char* on = std::getenv("XENON_ANOMALY_CAPTURE");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
  options.enabled = on && *on && !(on[0] == '0' && on[1] == '\0');
  options.max_count = static_cast<std::uint32_t>((std::min<std::uint64_t>)(
      number("XENON_ANOMALY_CAPTURE_COUNT", options.max_count), 1000u));
  options.cooldown_ms = number("XENON_ANOMALY_CAPTURE_COOLDOWN_MS", options.cooldown_ms);
  return options;
}

bool AnomalyCaptureGate::should_arm(std::uint64_t now_ms) {
  if (!options_.enabled) return false;
  std::scoped_lock lock(mutex_);
  if (armed_ >= options_.max_count) return false;
  if (have_last_ && now_ms < last_ms_ + options_.cooldown_ms) return false;
  ++armed_;
  have_last_ = true;
  last_ms_ = now_ms;
  return true;
}

std::uint32_t AnomalyCaptureGate::armed_count() const {
  std::scoped_lock lock(mutex_);
  return armed_;
}

}  // namespace xenon::core
