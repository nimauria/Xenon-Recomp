#include "xenon/filesystem/directory_cursor.hpp"

#include <algorithm>

namespace xenon::filesystem {

DirectoryCursor::DirectoryCursor(std::vector<DirectoryEntry> entries)
    : entries_(std::move(entries)) {}

FsError DirectoryCursor::read(std::size_t max_entries,
                              std::vector<DirectoryEntry>& out_entries,
                              bool& out_end) {
  std::scoped_lock lock(mutex_);
  out_entries.clear();

  if (position_ >= entries_.size()) {
    out_end = true;
    return FsError::None;
  }

  const auto remaining = entries_.size() - position_;
  const auto count = max_entries == 0 ? remaining : std::min(max_entries, remaining);
  out_entries.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    out_entries.push_back(entries_[position_ + i]);
  }
  position_ += count;
  out_end = position_ >= entries_.size();
  return FsError::None;
}

void DirectoryCursor::restart() {
  std::scoped_lock lock(mutex_);
  position_ = 0;
}

std::size_t DirectoryCursor::position() const {
  std::scoped_lock lock(mutex_);
  return position_;
}

std::size_t DirectoryCursor::size() const {
  std::scoped_lock lock(mutex_);
  return entries_.size();
}

bool DirectoryCursor::exhausted() const {
  std::scoped_lock lock(mutex_);
  return position_ >= entries_.size();
}

}  // namespace xenon::filesystem
