#include "xenon/filesystem/device.hpp"

#include <stdexcept>

#include "xenon/filesystem/path.hpp"

namespace xenon::filesystem {

Device::Device(std::string mount_point, bool read_only) : read_only_(read_only) {
  if (normalize_guest_path(mount_point, mount_point_, false) != FsError::None ||
      mount_point_.empty() || mount_point_.front() != '\\') {
    throw std::invalid_argument("filesystem device mount point must be an absolute guest path");
  }
}

}  // namespace xenon::filesystem
