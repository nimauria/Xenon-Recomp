#pragma once
#include <filesystem>
#include <memory>
namespace xenon::launcher_next {
class StorageLock {
public:
  explicit StorageLock(const std::filesystem::path &storage);
  ~StorageLock();
  StorageLock(const StorageLock &) = delete;
  StorageLock &operator=(const StorageLock &) = delete;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace xenon::launcher_next
