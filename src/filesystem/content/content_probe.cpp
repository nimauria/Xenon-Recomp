#include "xenon/filesystem/content_probe.hpp"
#include "xenon/filesystem/stfs_package_source.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <memory>
#include <limits>
#include <span>
#include <vector>

#include "xenon/filesystem/gdfx_image_source.hpp"
#include "xenon/filesystem/path.hpp"

namespace xenon::filesystem {
namespace {
constexpr std::size_t kXexBaseHeaderSize = 0x18;
constexpr std::size_t kMaxXexHeaderBytes = 16u * 1024u * 1024u;

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool extension_is(const std::filesystem::path& path, std::string_view extension) {
  return lower_ascii(path.extension().string()) == extension;
}

std::filesystem::path find_case_insensitive_child(
    const std::filesystem::path& directory, std::string_view name) {
  std::error_code ec;
  for (std::filesystem::directory_iterator it(directory, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (lower_ascii(it->path().filename().string()) == lower_ascii(std::string(name))) {
      return it->path();
    }
  }
  return {};
}

std::array<char, 4> file_magic(const std::filesystem::path& path) {
  std::array<char, 4> magic{};
  std::ifstream file(path, std::ios::binary);
  if (file) file.read(magic.data(), static_cast<std::streamsize>(magic.size()));
  return magic;
}

bool is_stfs_magic(const std::array<char, 4>& magic) {
  return magic == std::array<char, 4>{'C', 'O', 'N', ' '} ||
         magic == std::array<char, 4>{'L', 'I', 'V', 'E'} ||
         magic == std::array<char, 4>{'P', 'I', 'R', 'S'};
}

std::uint32_t read_be32(std::span<const std::byte> bytes, std::size_t offset) {
  return (std::to_integer<std::uint32_t>(bytes[offset]) << 24u) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16u) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8u) |
         std::to_integer<std::uint32_t>(bytes[offset + 3]);
}

FsError read_source_xex_metadata(const ReadOnlyContentSource& source,
                                 std::string_view path,
                                 XexMetadata& out_metadata) {
  out_metadata = {};
  FileInfo info{};
  auto error = source.stat(path, info);
  if (error != FsError::None) return error;
  if (info.is_directory || info.size < kXexBaseHeaderSize ||
      info.size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    return FsError::InvalidArgument;
  }

  std::array<std::byte, kXexBaseHeaderSize> base{};
  std::size_t bytes_read = 0;
  error = source.read_at(path, 0, base, bytes_read);
  if (error != FsError::None) return error;
  if (bytes_read != base.size() || base[0] != std::byte{'X'} ||
      base[1] != std::byte{'E'} || base[2] != std::byte{'X'} ||
      base[3] != std::byte{'2'}) {
    return FsError::InvalidArgument;
  }

  const auto header_size = static_cast<std::size_t>(read_be32(base, 8));
  if (header_size < kXexBaseHeaderSize || header_size > kMaxXexHeaderBytes ||
      header_size > info.size) {
    return FsError::InvalidArgument;
  }
  std::vector<std::byte> header(header_size);
  bytes_read = 0;
  error = source.read_at(path, 0, header, bytes_read);
  if (error != FsError::None) return error;
  if (bytes_read != header.size()) return FsError::IoError;
  return parse_xex_metadata(header, out_metadata);
}

std::string find_root_default_xex(const ReadOnlyContentSource& source) {
  std::vector<DirectoryEntry> entries;
  if (source.list({}, entries) != FsError::None) return {};
  for (const auto& entry : entries) {
    if (!entry.info.is_directory && guest_path_equal(entry.name, "default.xex")) {
      return entry.name;
    }
  }
  return {};
}
}  // namespace

std::string_view to_string(ContentSourceType type) noexcept {
  switch (type) {
    case ContentSourceType::Unknown: return "unknown";
    case ContentSourceType::Directory: return "directory";
    case ContentSourceType::Xex: return "xex";
    case ContentSourceType::GdfxImage: return "gdfx-image";
    case ContentSourceType::GdfxImageCandidate: return "gdfx-image-candidate";
    case ContentSourceType::StfsPackage: return "stfs-package";
    case ContentSourceType::StfsPackageCandidate: return "stfs-package-candidate";
  }
  return "unknown";
}

std::string_view to_string(ContentProbeStatus status) noexcept {
  switch (status) {
    case ContentProbeStatus::Identified: return "identified";
    case ContentProbeStatus::Candidate: return "candidate";
    case ContentProbeStatus::Unsupported: return "unsupported";
    case ContentProbeStatus::Invalid: return "invalid";
    case ContentProbeStatus::IoError: return "io-error";
  }
  return "invalid";
}

ContentProbeResult ContentProbe::probe(const std::filesystem::path& source) const {
  ContentProbeResult result{};
  result.source_path = source;

  std::error_code ec;
  const auto status = std::filesystem::symlink_status(source, ec);
  if (ec) {
    result.status = ContentProbeStatus::IoError;
    result.message = "Unable to inspect the selected content source.";
    return result;
  }
  if (std::filesystem::is_symlink(status)) {
    result.status = ContentProbeStatus::Invalid;
    result.message = "Content probe refuses symbolic-link roots.";
    return result;
  }
  if (std::filesystem::is_directory(status)) return probe_directory(source);
  if (std::filesystem::is_regular_file(status)) return probe_file(source);

  result.status = ContentProbeStatus::Unsupported;
  result.message = "Selected content is not a regular file or directory.";
  return result;
}

ContentProbeResult ContentProbe::probe_directory(
    const std::filesystem::path& source) const {
  ContentProbeResult result{};
  result.source_path = source;
  result.resolved_source_path = source;
  result.source_type = ContentSourceType::Directory;

  const auto executable = find_case_insensitive_child(source, "default.xex");
  if (executable.empty()) {
    result.status = ContentProbeStatus::Invalid;
    result.message = "Directory does not contain a root default.xex.";
    return result;
  }
  std::error_code executable_ec;
  const auto executable_status = std::filesystem::symlink_status(executable, executable_ec);
  if (executable_ec || std::filesystem::is_symlink(executable_status) ||
      !std::filesystem::is_regular_file(executable_status)) {
    result.status = ContentProbeStatus::Invalid;
    result.message = "default.xex must be a regular file inside the selected content root.";
    return result;
  }
  result.executable_path = executable;
  result.executable_guest_path = "game:\\" + executable.filename().string();
  const auto error = read_xex_metadata(executable, result.xex);
  if (error != FsError::None) {
    result.status = error == FsError::IoError ? ContentProbeStatus::IoError
                                               : ContentProbeStatus::Invalid;
    result.message = "default.xex header is not a supported valid XEX2 header.";
    return result;
  }
  if (!result.xex.execution_info) {
    result.status = ContentProbeStatus::Invalid;
    result.message = "default.xex does not expose XEX execution metadata.";
    return result;
  }

  result.status = ContentProbeStatus::Identified;
  result.message = "Xbox 360 title directory identified from default.xex.";
  return result;
}

ContentProbeResult ContentProbe::probe_file(
    const std::filesystem::path& source) const {
  ContentProbeResult result{};
  result.source_path = source;
  result.resolved_source_path = source;
  const auto magic = file_magic(source);

  if (magic == std::array<char, 4>{'X', 'E', 'X', '2'}) {
    result.source_type = ContentSourceType::Xex;
    result.executable_path = source;
    result.executable_guest_path = "game:\\" + source.filename().string();
    const auto error = read_xex_metadata(source, result.xex);
    if (error != FsError::None) {
      result.status = error == FsError::IoError ? ContentProbeStatus::IoError
                                                 : ContentProbeStatus::Invalid;
      result.message = "Selected XEX2 header is invalid or truncated.";
      return result;
    }
    if (!result.xex.execution_info) {
      result.status = ContentProbeStatus::Invalid;
      result.message = "Selected XEX2 does not expose execution metadata.";
      return result;
    }
    result.status = ContentProbeStatus::Identified;
    result.message = "Xbox 360 XEX2 executable identified.";
    return result;
  }

  if (magic == std::array<char, 4>{'X', 'E', 'X', '1'}) {
    result.source_type = ContentSourceType::Xex;
    result.status = ContentProbeStatus::Unsupported;
    result.message = "XEX1 was detected; metadata probing currently supports XEX2.";
    return result;
  }

  if (is_stfs_magic(magic)) {
    StfsPackageMetadata metadata{};
    const auto metadata_error = read_stfs_metadata(source, metadata);
    if (metadata_error != FsError::None) {
      result.source_type = ContentSourceType::StfsPackageCandidate;
      result.status = metadata_error == FsError::IoError
                          ? ContentProbeStatus::IoError
                          : ContentProbeStatus::Invalid;
      result.message = "STFS/XContent header is invalid or truncated.";
      return result;
    }
    result.stfs = metadata;
    if (metadata.volume_type != 0) {
      result.source_type = ContentSourceType::StfsPackageCandidate;
      result.status = ContentProbeStatus::Unsupported;
      result.message = "XContent package uses SVOD; Generation 6 supports single-file STFS payloads.";
      return result;
    }

    auto package = std::make_shared<StfsPackageSource>(source);
    const auto initialize_error = package->initialize();
    if (initialize_error != FsError::None) {
      result.source_type = ContentSourceType::StfsPackageCandidate;
      result.status = initialize_error == FsError::IoError
                          ? ContentProbeStatus::IoError
                          : ContentProbeStatus::Invalid;
      result.message = "STFS package filesystem is malformed or unsupported.";
      return result;
    }

    result.source_type = ContentSourceType::StfsPackage;
    result.status = ContentProbeStatus::Identified;
    result.message = "Xbox 360 STFS package identified and validated.";
    return result;
  }

  if (extension_is(source, ".iso") || extension_is(source, ".xgd") ||
      extension_is(source, ".dvd")) {
    result.source_type = ContentSourceType::GdfxImageCandidate;
    std::filesystem::path image_path;
    const auto resolve_error = resolve_gdfx_image_path(source, image_path);
    if (resolve_error != FsError::None) {
      result.status = resolve_error == FsError::IoError
                          ? ContentProbeStatus::IoError
                          : ContentProbeStatus::Invalid;
      result.message = "Disc descriptor does not reference a valid image path.";
      return result;
    }
    result.resolved_source_path = image_path;

    auto disc = std::make_shared<GdfxImageSource>(image_path);
    const auto initialize_error = disc->initialize();
    if (initialize_error != FsError::None) {
      result.status = initialize_error == FsError::IoError
                          ? ContentProbeStatus::IoError
                          : ContentProbeStatus::Invalid;
      result.message = "Selected disc image is not a supported valid GDFX image.";
      return result;
    }

    const auto executable_name = find_root_default_xex(*disc);
    if (executable_name.empty()) {
      result.status = ContentProbeStatus::Invalid;
      result.message = "GDFX image does not contain a root default.xex.";
      return result;
    }
    const auto xex_error =
        read_source_xex_metadata(*disc, executable_name, result.xex);
    if (xex_error != FsError::None || !result.xex.execution_info) {
      result.status = xex_error == FsError::IoError
                          ? ContentProbeStatus::IoError
                          : ContentProbeStatus::Invalid;
      result.message = "GDFX default.xex is not a supported valid XEX2 executable.";
      return result;
    }

    result.source_type = ContentSourceType::GdfxImage;
    result.executable_guest_path = "d:\\" + executable_name;
    result.status = ContentProbeStatus::Identified;
    result.message = "Xbox 360 GDFX disc image identified from root default.xex.";
    return result;
  }

  result.status = ContentProbeStatus::Unsupported;
  result.message = "Selected file is not recognized as XEX2, STFS, or a supported disc image.";
  return result;
}

}  // namespace xenon::filesystem
