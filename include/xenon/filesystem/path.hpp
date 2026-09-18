#pragma once

#include <string>
#include <string_view>

#include "xenon/filesystem/types.hpp"

namespace xenon::filesystem {

// Produces a canonical guest path using '\\' separators. Dot components are
// removed and attempts to walk above the supplied root are rejected.
[[nodiscard]] FsError normalize_guest_path(std::string_view input,
                                           std::string& output,
                                           bool allow_relative = true);

[[nodiscard]] bool guest_path_equal(std::string_view lhs,
                                    std::string_view rhs) noexcept;
[[nodiscard]] bool guest_path_has_prefix(std::string_view path,
                                         std::string_view prefix) noexcept;
[[nodiscard]] bool guest_path_is_absolute(std::string_view path) noexcept;
[[nodiscard]] std::string guest_path_key(std::string_view path);

}  // namespace xenon::filesystem
