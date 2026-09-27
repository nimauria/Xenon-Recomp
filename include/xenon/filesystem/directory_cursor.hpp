#pragma once

#include <cstddef>
#include <mutex>
#include <vector>

#include "xenon/filesystem/types.hpp"

namespace xenon::filesystem {

// Host-side resumable directory enumeration. The later Xbox kernel file object
// can own one of these and marshal batches into guest FILE_* structures without
// making the VFS aware of guest pointers or Memory.
class DirectoryCursor {
 public:
  explicit DirectoryCursor(std::vector<DirectoryEntry> entries);

  DirectoryCursor(const DirectoryCursor&) = delete;
  DirectoryCursor& operator=(const DirectoryCursor&) = delete;

  // Reads up to max_entries from the current cursor position. A value of zero
  // drains all remaining entries. out_end is true after the final entry has
  // been returned (or when the cursor was already exhausted).
  [[nodiscard]] FsError read(std::size_t max_entries,
                             std::vector<DirectoryEntry>& out_entries,
                             bool& out_end);
  void restart();

  [[nodiscard]] std::size_t position() const;
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] bool exhausted() const;

 private:
  mutable std::mutex mutex_{};
  std::vector<DirectoryEntry> entries_{};
  std::size_t position_{};
};

}  // namespace xenon::filesystem
