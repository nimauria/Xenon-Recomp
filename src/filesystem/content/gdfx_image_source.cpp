#include "xenon/filesystem/gdfx_image_source.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <string_view>

#include "xenon/filesystem/path.hpp"

namespace xenon::filesystem {
namespace {
constexpr std::uint64_t kSectorSize = 2048;
constexpr std::uint64_t kVolumeDescriptorSector = 32;
constexpr std::array<std::uint64_t, 5> kLikelyGameOffsets{
    0x00000000ull, 0x0000FB20ull, 0x00020600ull, 0x02080000ull,
    0x0FD90000ull};
constexpr std::string_view kMediaMagic = "MICROSOFT*XBOX*MEDIA";
constexpr std::uint32_t kMaxDirectoryBytes = 32u * 1024u * 1024u;
constexpr std::size_t kMaxDirectoryDepth = 64;
constexpr std::size_t kMaxNodes = 1'000'000;
constexpr std::size_t kEntryHeaderSize = 14;

std::uint16_t read_le16(std::span<const std::byte> bytes,
                        std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
      std::to_integer<std::uint8_t>(bytes[offset]) |
      (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1]))
       << 8u));
}

std::uint32_t read_le32(std::span<const std::byte> bytes,
                        std::size_t offset) noexcept {
  return std::to_integer<std::uint32_t>(bytes[offset]) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8u) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16u) |
         (std::to_integer<std::uint32_t>(bytes[offset + 3]) << 24u);
}

bool checked_add(std::uint64_t lhs, std::uint64_t rhs,
                 std::uint64_t& out) noexcept {
  if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) return false;
  out = lhs + rhs;
  return true;
}

bool checked_sector_offset(std::uint64_t base, std::uint32_t sector,
                           std::uint64_t& out) noexcept {
  const auto sector64 = static_cast<std::uint64_t>(sector);
  if (sector64 > (std::numeric_limits<std::uint64_t>::max() - base) /
                     kSectorSize) {
    return false;
  }
  out = base + sector64 * kSectorSize;
  return true;
}

std::uint64_t round_sector(std::uint64_t size) noexcept {
  if (size == 0) return 0;
  const auto remainder = size % kSectorSize;
  return remainder == 0 ? size : size + (kSectorSize - remainder);
}

std::string trim_ascii(std::string value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.erase(value.begin());
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.pop_back();
  }
  return value;
}

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}
}  // namespace

GdfxImageSource::GdfxImageSource(std::filesystem::path image_path)
    : image_path_(std::move(image_path)) {}

FsError GdfxImageSource::read_exact(std::ifstream& file, std::uint64_t offset,
                                    std::span<std::byte> destination) const {
  if (destination.empty()) return FsError::None;
  if (offset > info_.image_size ||
      destination.size() > info_.image_size - offset ||
      offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
    return FsError::InvalidArgument;
  }
  file.clear();
  file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!file) return FsError::IoError;
  file.read(reinterpret_cast<char*>(destination.data()),
            static_cast<std::streamsize>(destination.size()));
  return file.gcount() == static_cast<std::streamsize>(destination.size())
             ? FsError::None
             : FsError::IoError;
}

FsError GdfxImageSource::initialize() {
  initialized_ = false;
  nodes_.clear();
  path_index_.clear();
  info_ = {};
  info_.image_path = image_path_;

  std::error_code ec;
  const auto status = std::filesystem::symlink_status(image_path_, ec);
  if (ec || !std::filesystem::is_regular_file(status) ||
      std::filesystem::is_symlink(status)) {
    return FsError::InvalidArgument;
  }
  info_.image_size = std::filesystem::file_size(image_path_, ec);
  if (ec || info_.image_size < (kVolumeDescriptorSector + 1) * kSectorSize) {
    return FsError::InvalidArgument;
  }
  image_time_ = std::filesystem::last_write_time(image_path_, ec);
  if (ec) image_time_ = {};

  std::ifstream file(image_path_, std::ios::binary);
  if (!file) return FsError::IoError;

  std::array<std::byte, 32> descriptor{};
  bool found = false;
  for (const auto candidate : kLikelyGameOffsets) {
    std::uint64_t descriptor_offset{};
    if (!checked_add(candidate, kVolumeDescriptorSector * kSectorSize,
                     descriptor_offset) ||
        descriptor_offset > info_.image_size ||
        descriptor.size() > info_.image_size - descriptor_offset) {
      continue;
    }
    if (read_exact(file, descriptor_offset, descriptor) != FsError::None) {
      continue;
    }
    if (std::equal(kMediaMagic.begin(), kMediaMagic.end(), descriptor.begin(),
                   [](char lhs, std::byte rhs) {
                     return static_cast<unsigned char>(lhs) ==
                            std::to_integer<unsigned char>(rhs);
                   })) {
      info_.game_partition_offset = candidate;
      info_.root_sector = read_le32(descriptor, 20);
      info_.root_size = read_le32(descriptor, 24);
      found = true;
      break;
    }
  }
  if (!found) return FsError::InvalidArgument;
  if (info_.root_size < kEntryHeaderSize ||
      info_.root_size > kMaxDirectoryBytes) {
    return FsError::InvalidArgument;
  }

  std::uint64_t root_offset{};
  if (!checked_sector_offset(info_.game_partition_offset, info_.root_sector,
                             root_offset) ||
      root_offset > info_.image_size ||
      info_.root_size > info_.image_size - root_offset) {
    return FsError::InvalidArgument;
  }

  Node root{};
  root.info.is_directory = true;
  root.info.read_only = true;
  root.info.attributes = FileAttributeDirectory | FileAttributeReadOnly;
  root.info.last_write_time = image_time_;
  nodes_.push_back(std::move(root));
  path_index_.emplace("", 0);

  const auto parse_error =
      parse_directory(file, root_offset, info_.root_size, 0, 0);
  if (parse_error != FsError::None) {
    nodes_.clear();
    path_index_.clear();
    return parse_error;
  }

  initialized_ = true;
  return FsError::None;
}

FsError GdfxImageSource::parse_directory(std::ifstream& file,
                                         std::uint64_t directory_offset,
                                         std::uint32_t directory_size,
                                         std::size_t parent_index,
                                         std::size_t depth) {
  if (depth >= kMaxDirectoryDepth || directory_size < kEntryHeaderSize ||
      directory_size > kMaxDirectoryBytes || parent_index >= nodes_.size() ||
      directory_offset > info_.image_size ||
      directory_size > info_.image_size - directory_offset) {
    return FsError::InvalidArgument;
  }
  const auto ordinal_capacity =
      static_cast<std::size_t>(directory_size / 4u) + 1u;
  std::vector<std::uint8_t> visit_state(ordinal_capacity, 0);
  return parse_entry_tree(file, directory_offset, directory_size, 0,
                          parent_index, depth, visit_state);
}

FsError GdfxImageSource::parse_entry_tree(
    std::ifstream& file, std::uint64_t directory_offset,
    std::uint32_t directory_size, std::uint16_t ordinal,
    std::size_t parent_index, std::size_t depth,
    std::vector<std::uint8_t>& visit_state) {
  const auto relative_offset = static_cast<std::uint64_t>(ordinal) * 4u;
  if (relative_offset > directory_size ||
      kEntryHeaderSize > directory_size - relative_offset ||
      ordinal >= visit_state.size()) {
    return FsError::InvalidArgument;
  }
  if (visit_state[ordinal] != 0) return FsError::InvalidArgument;
  visit_state[ordinal] = 1;

  std::array<std::byte, kEntryHeaderSize> header{};
  const auto header_error =
      read_exact(file, directory_offset + relative_offset, header);
  if (header_error != FsError::None) return header_error;

  const auto left = read_le16(header, 0);
  const auto right = read_le16(header, 2);
  const auto sector = read_le32(header, 4);
  const auto length = read_le32(header, 8);
  const auto attributes = std::to_integer<std::uint8_t>(header[12]);
  const auto name_length = std::to_integer<std::uint8_t>(header[13]);
  if (name_length == 0 ||
      static_cast<std::uint64_t>(kEntryHeaderSize) + name_length >
          directory_size - relative_offset) {
    return FsError::InvalidArgument;
  }

  if (left != 0) {
    const auto error = parse_entry_tree(file, directory_offset, directory_size,
                                        left, parent_index, depth,
                                        visit_state);
    if (error != FsError::None) return error;
  }

  std::vector<std::byte> name_bytes(name_length);
  const auto name_error = read_exact(
      file, directory_offset + relative_offset + kEntryHeaderSize, name_bytes);
  if (name_error != FsError::None) return name_error;
  std::string name;
  name.reserve(name_length);
  for (const auto byte : name_bytes) {
    const auto c = static_cast<char>(std::to_integer<unsigned char>(byte));
    if (c == '\0' || c == '\\' || c == '/' || c == ':') {
      return FsError::InvalidArgument;
    }
    name.push_back(c);
  }
  if (name == "." || name == "..") return FsError::InvalidArgument;

  if (nodes_.size() >= kMaxNodes || parent_index >= nodes_.size()) {
    return FsError::InvalidArgument;
  }
  const auto parent_path = nodes_[parent_index].path;
  const auto full_path = parent_path.empty() ? name : parent_path + "\\" + name;
  const auto key = guest_path_key(full_path);
  if (path_index_.contains(key)) return FsError::InvalidArgument;

  Node node{};
  node.name = std::move(name);
  node.path = full_path;
  node.info.size = length;
  node.info.allocation_size = round_sector(length);
  node.info.is_directory = (attributes & FileAttributeDirectory) != 0;
  node.info.read_only = true;
  node.info.attributes = static_cast<std::uint32_t>(attributes) |
                         FileAttributeReadOnly;
  if (!node.info.is_directory &&
      (node.info.attributes & FileAttributeNormal) == 0) {
    node.info.attributes |= FileAttributeNormal;
  }
  node.info.last_write_time = image_time_;

  if (!node.info.is_directory) {
    if (!checked_sector_offset(info_.game_partition_offset, sector,
                               node.data_offset) ||
        node.data_offset > info_.image_size ||
        node.info.size > info_.image_size - node.data_offset) {
      return FsError::InvalidArgument;
    }
  }

  const auto node_index = nodes_.size();
  nodes_.push_back(std::move(node));
  path_index_.emplace(key, node_index);
  nodes_[parent_index].children.push_back(node_index);

  if (nodes_[node_index].info.is_directory && length != 0) {
    std::uint64_t child_offset{};
    if (!checked_sector_offset(info_.game_partition_offset, sector,
                               child_offset) ||
        child_offset > info_.image_size ||
        length > info_.image_size - child_offset) {
      return FsError::InvalidArgument;
    }
    const auto error = parse_directory(file, child_offset, length, node_index,
                                       depth + 1);
    if (error != FsError::None) return error;
  }

  if (right != 0) {
    const auto error = parse_entry_tree(file, directory_offset, directory_size,
                                        right, parent_index, depth,
                                        visit_state);
    if (error != FsError::None) return error;
  }

  visit_state[ordinal] = 2;
  return FsError::None;
}

FsError GdfxImageSource::normalize_relative(std::string_view relative_path,
                                            std::string& out_path) const {
  out_path.clear();
  if (relative_path.empty() || relative_path == "\\") return FsError::None;
  std::string normalized;
  const auto error = normalize_guest_path(relative_path, normalized, true);
  if (error != FsError::None) return error;
  if (normalized.find(':') != std::string::npos) return FsError::InvalidPath;
  while (!normalized.empty() && normalized.front() == '\\') {
    normalized.erase(normalized.begin());
  }
  out_path = std::move(normalized);
  return FsError::None;
}

const GdfxImageSource::Node* GdfxImageSource::find_node(
    std::string_view relative_path) const {
  if (!initialized_) return nullptr;
  std::string path;
  if (normalize_relative(relative_path, path) != FsError::None) return nullptr;
  const auto it = path_index_.find(guest_path_key(path));
  return it == path_index_.end() ? nullptr : &nodes_[it->second];
}

FsError GdfxImageSource::stat(std::string_view relative_path,
                              FileInfo& out_info) const {
  out_info = {};
  if (!initialized_) return FsError::IoError;
  const auto* node = find_node(relative_path);
  if (!node) return FsError::NotFound;
  out_info = node->info;
  return FsError::None;
}

FsError GdfxImageSource::list(
    std::string_view relative_path,
    std::vector<DirectoryEntry>& out_entries) const {
  out_entries.clear();
  if (!initialized_) return FsError::IoError;
  std::string path;
  const auto normalize_error = normalize_relative(relative_path, path);
  if (normalize_error != FsError::None) return normalize_error;
  const auto it = path_index_.find(guest_path_key(path));
  if (it == path_index_.end()) return FsError::NotFound;
  const auto& directory = nodes_[it->second];
  if (!directory.info.is_directory) return FsError::NotDirectory;
  out_entries.reserve(directory.children.size());
  for (const auto child_index : directory.children) {
    const auto& child = nodes_[child_index];
    out_entries.push_back(DirectoryEntry{child.name, child.info});
  }
  return FsError::None;
}

FsError GdfxImageSource::read_at(std::string_view relative_path,
                                 std::uint64_t offset,
                                 std::span<std::byte> destination,
                                 std::size_t& bytes_read) const {
  bytes_read = 0;
  if (!initialized_) return FsError::IoError;
  const auto* node = find_node(relative_path);
  if (!node) return FsError::NotFound;
  if (node->info.is_directory) return FsError::IsDirectory;
  if (offset >= node->info.size || destination.empty()) return FsError::None;

  const auto remaining = node->info.size - offset;
  const auto count = static_cast<std::size_t>(
      std::min<std::uint64_t>(remaining, destination.size()));
  std::ifstream file(image_path_, std::ios::binary);
  if (!file) return FsError::IoError;
  const auto error = read_exact(file, node->data_offset + offset,
                                destination.first(count));
  if (error != FsError::None) return error;
  bytes_read = count;
  return FsError::None;
}

FsError GdfxImageSource::disk_space(DiskSpace& out_space) const {
  out_space = {};
  if (!initialized_) return FsError::IoError;
  out_space.capacity = info_.image_size;
  out_space.free = 0;
  out_space.available = 0;
  return FsError::None;
}

FsError resolve_gdfx_image_path(const std::filesystem::path& source,
                                std::filesystem::path& out_image_path) {
  out_image_path.clear();
  auto extension = lower_ascii(source.extension().string());
  if (extension != ".dvd") {
    out_image_path = source;
    return FsError::None;
  }

  std::ifstream descriptor(source);
  if (!descriptor) return FsError::IoError;
  std::string line;
  while (std::getline(descriptor, line)) {
    line = trim_ascii(std::move(line));
    if (line.empty() || line.front() == '#' || line.front() == ';') continue;
    const auto lowered = lower_ascii(line);
    if (lowered.rfind("layerbreak=", 0) == 0 ||
        lowered.rfind("layer_break=", 0) == 0) {
      continue;
    }
    if (line.size() >= 2 &&
        ((line.front() == '"' && line.back() == '"') ||
         (line.front() == '\'' && line.back() == '\''))) {
      line = line.substr(1, line.size() - 2);
    }
    std::filesystem::path referenced(line);
    if (referenced.is_relative()) referenced = source.parent_path() / referenced;
    out_image_path = referenced.lexically_normal();
    return FsError::None;
  }
  return FsError::InvalidArgument;
}

}  // namespace xenon::filesystem
