#include "storage_lock.hpp"
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
namespace xenon::launcher_next {
struct StorageLock::Impl {
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE;
  ~Impl() {
    if (handle != INVALID_HANDLE_VALUE)
      CloseHandle(handle);
  }
#else
  int handle = -1;
  ~Impl() {
    if (handle >= 0)
      close(handle);
  }
#endif
};
StorageLock::StorageLock(const std::filesystem::path &storage) : impl_(std::make_unique<Impl>()) {
  std::filesystem::create_directories(storage);
  const auto file = storage / "instance.lock";
#ifdef _WIN32
  impl_->handle = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (impl_->handle == INVALID_HANDLE_VALUE)
#else
  impl_->handle = open(file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (impl_->handle < 0 || flock(impl_->handle, LOCK_EX | LOCK_NB) != 0)
#endif
    throw std::runtime_error("This launcher data folder is already open or cannot be locked. Close "
                             "the other launcher instance before trying again.");
}
StorageLock::~StorageLock() = default;
} // namespace xenon::launcher_next
