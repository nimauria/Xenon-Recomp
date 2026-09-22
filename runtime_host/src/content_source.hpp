#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace xenon::runtime_host {

// Normalized view of user-provided base game content. The launcher may hand
// the runtime a game directory, a loose XEX, or an Xbox 360 disc image. This
// structure keeps the executable bytes and the path that should back game:/
// together so those inputs all converge on the same XenonSession flow.
struct BaseContent {
  std::vector<std::byte> xex_bytes{};
  std::filesystem::path mount_path{};
  std::string executable_description{};
};

// Returns true for content source shapes the runtime host knows how to
// normalize. Existence/validity is checked by load_base_content().
[[nodiscard]] bool is_supported_base_content_path(const std::filesystem::path& path);

// Loads root default.xex bytes without extracting a whole disc image. For a
// loose XEX, mount_path becomes its containing directory; for .dvd it becomes
// the resolved image named by the descriptor. `error` is human-readable and
// safe for launcher status reporting.
[[nodiscard]] bool load_base_content(const std::filesystem::path& path,
                                     BaseContent& out,
                                     std::string& error);

}  // namespace xenon::runtime_host
