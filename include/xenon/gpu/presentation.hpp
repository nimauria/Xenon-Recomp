#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xenon/gpu/resource_ir.hpp"

namespace xenon::gpu {

// Presentation is deliberately outside the Xenos command stream. The runtime
// supplies the scanout/front-buffer resource and a native host surface/window;
// the launcher UI never participates in renderer presentation.
struct PresentationConfig {
  std::uint32_t width{1280};
  std::uint32_t height{720};
  std::uint32_t image_count{3};
  bool vsync{true};
  bool allow_tearing{true};
  bool preserve_aspect_ratio{true};
};

struct PresentationFrame {
  TextureDescriptor texture{};
  // Zero means use the texture dimensions. These fields allow a scanout image
  // to expose only the visible part of a larger pitch-aligned allocation.
  std::uint32_t visible_width{};
  std::uint32_t visible_height{};
};

enum class PresentStatus : std::uint8_t {
  Success,
  Suboptimal,
  OutOfDate,
  SurfaceLost,
  Minimized,
  NotConfigured,
  Unsupported,
  Error,
};

struct PreparedPresentationFrame {
  std::vector<std::byte> rgba8{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t row_pitch{};
  bool valid{};
  std::string error{};
};

// Converts the base 2D Xenos scanout resource to tightly packed RGBA8 and
// scales/letterboxes it to the requested host extent. This is the correctness
// path for GPU v1. Native backends may later replace it with a fully GPU-side
// scanout shader without changing the public presentation contract.
[[nodiscard]] PreparedPresentationFrame prepare_presentation_frame(
    const PresentationFrame& frame, std::span<const std::byte> physical_memory,
    std::uint32_t target_width, std::uint32_t target_height,
    bool preserve_aspect_ratio = true, std::uint32_t physical_base = 0u);

}  // namespace xenon::gpu
