#pragma once

#include <filesystem>

#include "xenon/filesystem/read_only_content_device.hpp"

namespace xenon::filesystem {

// Safely extracts an immutable content source into an already-chosen host
// destination. Package paths are treated as guest-relative paths and never
// used directly as host absolute paths.
[[nodiscard]] FsError materialize_content_source(
    const ReadOnlyContentSource& source,
    const std::filesystem::path& destination);

}  // namespace xenon::filesystem
