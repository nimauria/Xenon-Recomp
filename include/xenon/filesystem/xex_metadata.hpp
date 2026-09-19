#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

#include "xenon/filesystem/types.hpp"

namespace xenon::filesystem {

struct XexVersion {
  std::uint32_t value{};
  std::uint8_t major{};
  std::uint8_t minor{};
  std::uint16_t build{};
  std::uint8_t qfe{};

  [[nodiscard]] std::string to_string() const;
};

struct XexExecutionInfo {
  std::uint32_t media_id{};
  XexVersion version{};
  XexVersion base_version{};
  std::uint32_t title_id{};
  std::uint8_t platform{};
  std::uint8_t executable_table{};
  std::uint8_t disc_number{};
  std::uint8_t disc_count{};
  std::uint32_t savegame_id{};
};

struct XexMetadata {
  std::uint32_t module_flags{};
  std::uint32_t header_size{};
  std::uint32_t security_offset{};
  std::uint32_t optional_header_count{};
  std::optional<XexExecutionInfo> execution_info{};
  std::string original_pe_name{};
};

[[nodiscard]] std::string format_xbox_id(std::uint32_t value);

// Metadata-only XEX2 parser. This intentionally does not decrypt or map the
// executable image: launcher/content identification only needs the unencrypted
// header and optional execution metadata.
[[nodiscard]] FsError parse_xex_metadata(std::span<const std::byte> bytes,
                                         XexMetadata& out_metadata);
[[nodiscard]] FsError read_xex_metadata(const std::filesystem::path& path,
                                        XexMetadata& out_metadata);

}  // namespace xenon::filesystem
