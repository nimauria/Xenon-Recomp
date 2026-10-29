#pragma once

#include <filesystem>

#include "xenon/gpu/presentation.hpp"

namespace xenon::core {

// Writes a captured RGBA8 scan-out image as a top-down 32-bit BMP, the
// format every image viewer opens without a codec. False if `frame` is not a
// valid capture or the file cannot be written.
[[nodiscard]] bool write_frame_bmp(const std::filesystem::path& path,
                                   const gpu::PreparedPresentationFrame& frame);

}  // namespace xenon::core
