#include "xenon/filesystem/null_device.hpp"

namespace xenon::filesystem {

NullDevice::NullDevice(std::string mount_point)
    : Device(std::move(mount_point), true) {}

FsError NullDevice::initialize() { return FsError::None; }

FsError NullDevice::stat(std::string_view relative_path,
                         FileInfo& out_info) const {
  out_info = {};
  if (relative_path.empty()) {
    out_info.is_directory = true;
    out_info.read_only = true;
    out_info.attributes = FileAttributeDirectory | FileAttributeReadOnly;
    return FsError::None;
  }
  return FsError::NotFound;
}

FsError NullDevice::open(std::string_view, const OpenOptions&,
                         std::unique_ptr<FileHandle>& out_file,
                         OpenAction* out_action) {
  out_file.reset();
  if (out_action) *out_action = OpenAction::None;
  return FsError::NotFound;
}

FsError NullDevice::list(std::string_view relative_path,
                         std::vector<DirectoryEntry>& out_entries) const {
  out_entries.clear();
  return relative_path.empty() ? FsError::None : FsError::NotFound;
}

FsError NullDevice::create_directory(std::string_view, bool) {
  return FsError::ReadOnly;
}

FsError NullDevice::remove(std::string_view) { return FsError::ReadOnly; }

FsError NullDevice::rename(std::string_view, std::string_view, bool) {
  return FsError::ReadOnly;
}

FsError NullDevice::disk_space(DiskSpace& out_space) const {
  out_space = {};
  return FsError::None;
}

}  // namespace xenon::filesystem
