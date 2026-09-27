#include "xenon/filesystem/stfs_package_source.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <unordered_set>

#include "xenon/filesystem/path.hpp"

namespace xenon::filesystem {
namespace {
constexpr std::uint64_t kBlockSize = 0x1000;
constexpr std::uint32_t kBlocksPerHashLevel0 = 170;
constexpr std::uint32_t kBlocksPerHashLevel1 = 28900;
constexpr std::uint32_t kEndOfChain = 0xFFFFFF;
constexpr std::size_t kDirectoryEntrySize = 0x40;
constexpr std::size_t kEntriesPerDirectoryBlock = 0x40;
constexpr std::size_t kMaxNodes = 1'000'000;
constexpr std::size_t kMaxChainBlocks = 4'913'000;

std::uint16_t read_be16(std::span<const std::byte> bytes,
                        std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(
      (std::to_integer<std::uint16_t>(bytes[offset]) << 8u) |
      std::to_integer<std::uint16_t>(bytes[offset + 1]));
}

std::uint32_t read_be32(std::span<const std::byte> bytes,
                        std::size_t offset) noexcept {
  return (std::to_integer<std::uint32_t>(bytes[offset]) << 24u) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 16u) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 8u) |
         std::to_integer<std::uint32_t>(bytes[offset + 3]);
}

std::uint32_t read_le24(std::span<const std::byte> bytes,
                        std::size_t offset) noexcept {
  return std::to_integer<std::uint32_t>(bytes[offset]) |
         (std::to_integer<std::uint32_t>(bytes[offset + 1]) << 8u) |
         (std::to_integer<std::uint32_t>(bytes[offset + 2]) << 16u);
}

std::uint64_t round_up(std::uint64_t value, std::uint64_t alignment) noexcept {
  if (alignment == 0) return value;
  const auto rem = value % alignment;
  if (!rem) return value;
  const auto add = alignment - rem;
  return value <= std::numeric_limits<std::uint64_t>::max() - add ? value + add
                                                                  : 0;
}

std::filesystem::file_time_type fat_time(std::uint16_t date,
                                         std::uint16_t time) noexcept {
  const int year = 1980 + ((date >> 9u) & 0x7Fu);
  const unsigned month = (date >> 5u) & 0x0Fu;
  const unsigned day = date & 0x1Fu;
  const unsigned hour = (time >> 11u) & 0x1Fu;
  const unsigned minute = (time >> 5u) & 0x3Fu;
  const unsigned second = (time & 0x1Fu) * 2u;
  if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 ||
      minute > 59 || second > 59) {
    return {};
  }

  using namespace std::chrono;
  const year_month_day ymd{std::chrono::year{year}, std::chrono::month{month},
                           std::chrono::day{day}};
  if (!ymd.ok()) return {};
  const auto sys = sys_days{ymd} + hours{hour} + minutes{minute} + seconds{second};
  return std::filesystem::file_time_type::clock::now() +
         duration_cast<std::filesystem::file_time_type::duration>(
             sys - system_clock::now());
}

bool valid_entry_name(std::string_view name) noexcept {
  if (name.empty() || name == "." || name == "..") return false;
  for (const unsigned char c : name) {
    if (c == 0 || c == '/' || c == '\\' || c < 0x20u) return false;
  }
  return true;
}
}  // namespace

StfsPackageSource::StfsPackageSource(std::filesystem::path package_path)
    : package_path_(std::move(package_path)) {}

FsError StfsPackageSource::read_exact(std::ifstream& file, std::uint64_t offset,
                                      std::span<std::byte> destination) const {
  if (destination.empty()) return FsError::None;
  if (offset > info_.package_size || destination.size() > info_.package_size - offset ||
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

bool StfsPackageSource::block_to_offset(std::uint64_t block_index,
                                        std::uint64_t& out_offset) const noexcept {
  if (block_index >= info_.metadata.volume.total_block_count) return false;
  std::uint64_t base = kBlocksPerHashLevel0;
  std::uint64_t physical_block = block_index;
  for (std::uint32_t level = 0; level < 3; ++level) {
    const auto quotient = (block_index + base) / base;
    if (quotient > (std::numeric_limits<std::uint64_t>::max() - physical_block) /
                       blocks_per_hash_table_) {
      return false;
    }
    physical_block += quotient * blocks_per_hash_table_;
    if (block_index < base) break;
    if (base > std::numeric_limits<std::uint64_t>::max() / kBlocksPerHashLevel0) {
      return false;
    }
    base *= kBlocksPerHashLevel0;
  }

  const auto data_base = round_up(info_.metadata.header_size, kBlockSize);
  if (!data_base || physical_block >
                        (std::numeric_limits<std::uint64_t>::max() - data_base) /
                            kBlockSize) {
    return false;
  }
  out_offset = data_base + physical_block * kBlockSize;
  return out_offset <= info_.package_size && kBlockSize <= info_.package_size - out_offset;
}

bool StfsPackageSource::hash_block_number(std::uint32_t block_index,
                                          std::uint32_t hash_level,
                                          std::uint32_t& out_block) const noexcept {
  if (hash_level > 2) return false;
  std::uint64_t block = 0;
  if (hash_level == 0) {
    if (block_index < kBlocksPerHashLevel0) {
      out_block = 0;
      return true;
    }
    block = static_cast<std::uint64_t>(block_index / kBlocksPerHashLevel0) *
            block_step_[0];
    block += (static_cast<std::uint64_t>(block_index / kBlocksPerHashLevel1) + 1) *
             blocks_per_hash_table_;
    if (block_index >= kBlocksPerHashLevel1) block += blocks_per_hash_table_;
  } else if (hash_level == 1) {
    if (block_index < kBlocksPerHashLevel1) {
      out_block = block_step_[0];
      return true;
    }
    block = static_cast<std::uint64_t>(block_index / kBlocksPerHashLevel1) *
                block_step_[1] +
            blocks_per_hash_table_;
  } else {
    block = block_step_[1];
  }
  if (block > std::numeric_limits<std::uint32_t>::max()) return false;
  out_block = static_cast<std::uint32_t>(block);
  return true;
}

bool StfsPackageSource::hash_block_offset(std::uint32_t block_index,
                                          std::uint32_t hash_level,
                                          std::uint64_t& out_offset) const noexcept {
  std::uint32_t hash_block = 0;
  if (!hash_block_number(block_index, hash_level, hash_block)) return false;
  const auto base = round_up(info_.metadata.header_size, kBlockSize);
  if (!base || hash_block >
                   (std::numeric_limits<std::uint64_t>::max() - base) / kBlockSize) {
    return false;
  }
  out_offset = base + static_cast<std::uint64_t>(hash_block) * kBlockSize;
  return out_offset <= info_.package_size && kBlockSize <= info_.package_size - out_offset;
}

FsError StfsPackageSource::read_hash_entry(std::ifstream& file,
                                           std::uint32_t block_index,
                                           std::uint32_t& out_next_block) const {
  if (block_index >= info_.metadata.volume.total_block_count) {
    return FsError::InvalidArgument;
  }

  std::uint64_t secondary = info_.metadata.volume.root_active_index ? kBlockSize : 0;
  std::array<std::byte, 0x18> entry{};

  auto read_table_entry = [&](std::uint64_t table_offset, std::uint32_t record,
                              std::uint32_t& info_raw) -> FsError {
    if (record >= kBlocksPerHashLevel0) return FsError::InvalidArgument;
    const auto offset = table_offset + secondary +
                        static_cast<std::uint64_t>(record) * entry.size();
    const auto error = read_exact(file, offset, entry);
    if (error != FsError::None) return error;
    info_raw = read_be32(entry, 0x14);
    return FsError::None;
  };

  if (info_.metadata.volume.read_only_format) {
    secondary = 0;
  } else if (info_.metadata.volume.total_block_count > kBlocksPerHashLevel0) {
    if (info_.metadata.volume.total_block_count > kBlocksPerHashLevel1) {
      std::uint64_t level2_offset = 0;
      if (!hash_block_offset(block_index, 2, level2_offset)) {
        return FsError::InvalidArgument;
      }
      std::uint32_t info_raw = 0;
      const auto record = (block_index / kBlocksPerHashLevel1) % kBlocksPerHashLevel0;
      const auto error = read_table_entry(level2_offset, record, info_raw);
      if (error != FsError::None) return error;
      secondary = (info_raw & 0x40000000u) ? kBlockSize : 0;
    }

    std::uint64_t level1_offset = 0;
    if (!hash_block_offset(block_index, 1, level1_offset)) {
      return FsError::InvalidArgument;
    }
    std::uint32_t info_raw = 0;
    const auto record = (block_index / kBlocksPerHashLevel0) % kBlocksPerHashLevel0;
    const auto error = read_table_entry(level1_offset, record, info_raw);
    if (error != FsError::None) return error;
    secondary = (info_raw & 0x40000000u) ? kBlockSize : 0;
  }

  std::uint64_t level0_offset = 0;
  if (!hash_block_offset(block_index, 0, level0_offset)) {
    return FsError::InvalidArgument;
  }
  std::uint32_t info_raw = 0;
  const auto error = read_table_entry(level0_offset, block_index % kBlocksPerHashLevel0,
                                      info_raw);
  if (error != FsError::None) return error;
  out_next_block = info_raw & 0x00FFFFFFu;
  return FsError::None;
}

FsError StfsPackageSource::build_block_chain(
    std::ifstream& file, std::uint32_t start_block,
    std::uint32_t allocated_blocks, std::uint64_t logical_size,
    std::vector<std::uint32_t>& out_blocks) const {
  out_blocks.clear();
  if (logical_size == 0) return FsError::None;
  if (start_block >= info_.metadata.volume.total_block_count) {
    return FsError::InvalidArgument;
  }

  const auto required_blocks =
      static_cast<std::uint64_t>((logical_size + kBlockSize - 1) / kBlockSize);
  if (required_blocks > kMaxChainBlocks ||
      required_blocks > info_.metadata.volume.total_block_count) {
    return FsError::InvalidArgument;
  }
  if (allocated_blocks != 0 && allocated_blocks < required_blocks) {
    return FsError::InvalidArgument;
  }

  std::unordered_set<std::uint32_t> visited;
  visited.reserve(static_cast<std::size_t>(required_blocks));
  auto block = start_block;
  for (std::uint64_t i = 0; i < required_blocks; ++i) {
    if (block == kEndOfChain || block >= info_.metadata.volume.total_block_count ||
        !visited.insert(block).second) {
      return FsError::InvalidArgument;
    }
    std::uint64_t offset = 0;
    if (!block_to_offset(block, offset)) return FsError::InvalidArgument;
    out_blocks.push_back(block);
    if (i + 1 == required_blocks) break;
    std::uint32_t next = 0;
    const auto error = read_hash_entry(file, block, next);
    if (error != FsError::None) return error;
    block = next;
  }
  return FsError::None;
}

FsError StfsPackageSource::parse_file_table(std::ifstream& file) {
  nodes_.clear();
  path_index_.clear();

  Node root{};
  root.path.clear();
  root.info.is_directory = true;
  root.info.read_only = true;
  root.info.attributes = FileAttributeDirectory | FileAttributeReadOnly;
  root.info.last_write_time = package_time_;
  nodes_.push_back(std::move(root));
  path_index_.emplace("", 0);

  const auto& volume = info_.metadata.volume;
  if (volume.file_table_block_count == 0 ||
      volume.file_table_block_number >= volume.total_block_count) {
    return FsError::InvalidArgument;
  }

  std::vector<std::size_t> directory_entry_nodes;
  directory_entry_nodes.reserve(
      static_cast<std::size_t>(volume.file_table_block_count) *
      kEntriesPerDirectoryBlock);
  std::unordered_set<std::uint32_t> table_blocks;
  auto table_block = volume.file_table_block_number;

  std::array<std::byte, kBlockSize> block_bytes{};
  for (std::uint32_t table_index = 0;
       table_index < volume.file_table_block_count; ++table_index) {
    if (table_block == kEndOfChain || table_block >= volume.total_block_count ||
        !table_blocks.insert(table_block).second) {
      return FsError::InvalidArgument;
    }
    std::uint64_t table_offset = 0;
    if (!block_to_offset(table_block, table_offset)) return FsError::InvalidArgument;
    const auto read_error = read_exact(file, table_offset, block_bytes);
    if (read_error != FsError::None) return read_error;

    for (std::size_t entry_index = 0; entry_index < kEntriesPerDirectoryBlock;
         ++entry_index) {
      const auto offset = entry_index * kDirectoryEntrySize;
      if (block_bytes[offset] == std::byte{0}) break;
      const auto flags = std::to_integer<std::uint8_t>(block_bytes[offset + 0x28]);
      const auto name_length = static_cast<std::size_t>(flags & 0x3Fu);
      if (name_length == 0 || name_length > 40) return FsError::InvalidArgument;

      std::string name;
      name.reserve(name_length);
      for (std::size_t i = 0; i < name_length; ++i) {
        name.push_back(static_cast<char>(
            std::to_integer<unsigned char>(block_bytes[offset + i])));
      }
      if (!valid_entry_name(name)) return FsError::InvalidArgument;

      const auto parent_ordinal = read_be16(block_bytes, offset + 0x32);
      std::size_t parent_node = 0;
      if (parent_ordinal != 0xFFFFu) {
        if (parent_ordinal >= directory_entry_nodes.size()) {
          return FsError::InvalidArgument;
        }
        parent_node = directory_entry_nodes[parent_ordinal];
        if (!nodes_[parent_node].info.is_directory) return FsError::InvalidArgument;
      }

      Node node{};
      node.name = name;
      node.path = nodes_[parent_node].path.empty()
                      ? name
                      : nodes_[parent_node].path + "\\" + name;
      if (path_index_.contains(guest_path_key(node.path))) {
        return FsError::AlreadyExists;
      }
      node.info.is_directory = (flags & 0x80u) != 0;
      node.info.read_only = true;
      node.info.attributes = (node.info.is_directory ? FileAttributeDirectory
                                                     : FileAttributeNormal) |
                             FileAttributeReadOnly;
      node.info.size = read_be32(block_bytes, offset + 0x34);
      node.info.allocation_size =
          round_up(node.info.size, static_cast<std::uint64_t>(kBlockSize));
      node.info.last_write_time = fat_time(read_be16(block_bytes, offset + 0x3C),
                                           read_be16(block_bytes, offset + 0x3E));
      if (node.info.last_write_time == std::filesystem::file_time_type{}) {
        node.info.last_write_time = package_time_;
      }

      if (!node.info.is_directory) {
        const auto allocated = read_le24(block_bytes, offset + 0x2C);
        const auto start = read_le24(block_bytes, offset + 0x2F);
        const auto chain_error =
            build_block_chain(file, start, allocated, node.info.size, node.blocks);
        if (chain_error != FsError::None) return chain_error;
      }

      if (nodes_.size() >= kMaxNodes) return FsError::InvalidArgument;
      const auto node_index = nodes_.size();
      nodes_.push_back(std::move(node));
      nodes_[parent_node].children.push_back(node_index);
      directory_entry_nodes.push_back(node_index);
      path_index_.emplace(guest_path_key(nodes_[node_index].path), node_index);
    }

    if (table_index + 1 < volume.file_table_block_count) {
      std::uint32_t next = 0;
      const auto error = read_hash_entry(file, table_block, next);
      if (error != FsError::None) return error;
      table_block = next;
    }
  }
  return FsError::None;
}

FsError StfsPackageSource::initialize() {
  initialized_ = false;
  info_ = {};
  nodes_.clear();
  path_index_.clear();
  info_.package_path = package_path_;

  std::error_code ec;
  const auto status = std::filesystem::symlink_status(package_path_, ec);
  if (ec || std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    return FsError::InvalidArgument;
  }
  info_.package_size = std::filesystem::file_size(package_path_, ec);
  if (ec) return FsError::IoError;
  package_time_ = std::filesystem::last_write_time(package_path_, ec);
  if (ec) package_time_ = {};

  const auto metadata_error = read_stfs_metadata(package_path_, info_.metadata);
  if (metadata_error != FsError::None) return metadata_error;
  if (info_.metadata.volume_type != 0) return FsError::Unsupported;
  if (info_.metadata.volume.total_block_count == 0) return FsError::InvalidArgument;

  blocks_per_hash_table_ = info_.metadata.volume.read_only_format ? 1u : 2u;
  block_step_[0] = kBlocksPerHashLevel0 + blocks_per_hash_table_;
  block_step_[1] = kBlocksPerHashLevel1 +
                   ((kBlocksPerHashLevel0 + 1u) * blocks_per_hash_table_);

  std::ifstream file(package_path_, std::ios::binary);
  if (!file) return FsError::IoError;
  const auto table_error = parse_file_table(file);
  if (table_error != FsError::None) return table_error;
  initialized_ = true;
  return FsError::None;
}

FsError StfsPackageSource::normalize_relative(std::string_view relative_path,
                                              std::string& out_path) const {
  if (relative_path.empty()) {
    out_path.clear();
    return FsError::None;
  }
  const auto error = normalize_guest_path(relative_path, out_path, true);
  if (error != FsError::None || guest_path_is_absolute(out_path)) {
    return FsError::InvalidPath;
  }
  return FsError::None;
}

const StfsPackageSource::Node* StfsPackageSource::find_node(
    std::string_view relative_path) const {
  std::string normalized;
  if (normalize_relative(relative_path, normalized) != FsError::None) return nullptr;
  const auto it = path_index_.find(guest_path_key(normalized));
  return it == path_index_.end() ? nullptr : &nodes_[it->second];
}

FsError StfsPackageSource::stat(std::string_view relative_path,
                                FileInfo& out_info) const {
  out_info = {};
  if (!initialized_) return FsError::IoError;
  const auto* node = find_node(relative_path);
  if (!node) return FsError::NotFound;
  out_info = node->info;
  return FsError::None;
}

FsError StfsPackageSource::list(
    std::string_view relative_path,
    std::vector<DirectoryEntry>& out_entries) const {
  out_entries.clear();
  if (!initialized_) return FsError::IoError;
  const auto* node = find_node(relative_path);
  if (!node) return FsError::NotFound;
  if (!node->info.is_directory) return FsError::NotDirectory;
  for (const auto child_index : node->children) {
    const auto& child = nodes_[child_index];
    out_entries.push_back({child.name, child.info});
  }
  std::sort(out_entries.begin(), out_entries.end(), [](const auto& lhs, const auto& rhs) {
    return guest_path_key(lhs.name) < guest_path_key(rhs.name);
  });
  return FsError::None;
}

FsError StfsPackageSource::read_at(std::string_view relative_path,
                                   std::uint64_t offset,
                                   std::span<std::byte> destination,
                                   std::size_t& bytes_read) const {
  bytes_read = 0;
  if (!initialized_) return FsError::IoError;
  const auto* node = find_node(relative_path);
  if (!node) return FsError::NotFound;
  if (node->info.is_directory) return FsError::IsDirectory;
  if (destination.empty() || offset >= node->info.size) return FsError::None;

  const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
      node->info.size - offset, static_cast<std::uint64_t>(destination.size())));
  std::ifstream file(package_path_, std::ios::binary);
  if (!file) return FsError::IoError;

  auto logical = offset;
  while (bytes_read < count) {
    const auto block_ordinal = static_cast<std::size_t>(logical / kBlockSize);
    const auto block_offset = static_cast<std::size_t>(logical % kBlockSize);
    if (block_ordinal >= node->blocks.size()) return FsError::InvalidArgument;
    std::uint64_t physical = 0;
    if (!block_to_offset(node->blocks[block_ordinal], physical)) {
      return FsError::InvalidArgument;
    }
    const auto chunk = std::min<std::size_t>(
        count - bytes_read, static_cast<std::size_t>(kBlockSize) - block_offset);
    const auto error = read_exact(
        file, physical + block_offset,
        destination.subspan(bytes_read, chunk));
    if (error != FsError::None) return error;
    bytes_read += chunk;
    logical += chunk;
  }
  return FsError::None;
}

FsError StfsPackageSource::disk_space(DiskSpace& out_space) const {
  out_space = {};
  if (!initialized_) return FsError::IoError;
  out_space.capacity = static_cast<std::uint64_t>(
      info_.metadata.volume.total_block_count) * kBlockSize;
  out_space.free = 0;
  out_space.available = 0;
  return FsError::None;
}

}  // namespace xenon::filesystem
