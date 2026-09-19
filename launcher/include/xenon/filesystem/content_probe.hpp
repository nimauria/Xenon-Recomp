#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "xenon/filesystem/stfs_package.hpp"
#include "xenon/filesystem/xex_metadata.hpp"

namespace xenon::filesystem {

enum class ContentSourceType : std::uint8_t {
  Unknown = 0,
  Directory,
  Xex,
  GdfxImage,
  GdfxImageCandidate,
  StfsPackage,
  StfsPackageCandidate,
};

enum class ContentProbeStatus : std::uint8_t {
  Identified = 0,
  Candidate,
  Unsupported,
  Invalid,
  IoError,
};

struct ContentProbeResult {
  ContentProbeStatus status{ContentProbeStatus::Invalid};
  ContentSourceType source_type{ContentSourceType::Unknown};
  std::filesystem::path source_path{};
  std::filesystem::path executable_path{};
  std::filesystem::path resolved_source_path{};
  std::string executable_guest_path{};
  XexMetadata xex{};
  std::optional<StfsPackageMetadata> stfs{};
  std::string message{};

  [[nodiscard]] bool identified() const noexcept {
    return status == ContentProbeStatus::Identified;
  }

  [[nodiscard]] bool has_game_identity() const noexcept {
    return identified() && xex.execution_info.has_value();
  }
};

[[nodiscard]] std::string_view to_string(ContentSourceType type) noexcept;
[[nodiscard]] std::string_view to_string(ContentProbeStatus status) noexcept;

class ContentProbe {
 public:
  [[nodiscard]] ContentProbeResult probe(
      const std::filesystem::path& source) const;

 private:
  [[nodiscard]] ContentProbeResult probe_directory(
      const std::filesystem::path& source) const;
  [[nodiscard]] ContentProbeResult probe_file(
      const std::filesystem::path& source) const;
};

}  // namespace xenon::filesystem
