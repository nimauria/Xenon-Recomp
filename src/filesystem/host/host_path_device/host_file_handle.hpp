#pragma once

// File handles for files opened through HostPathDevice: a portable
// std::fstream-based handle and, on Windows, a CreateFileW handle that can
// grant FILE_SHARE_DELETE. Included only by host_path_device.cpp.

#include "filesystem/host/host_path_device/host_path_internal.hpp"

namespace xenon::filesystem {

class HostFileHandle final : public FileHandle {
 public:
  HostFileHandle(std::filesystem::path path, FileAccess access,
                 std::fstream stream, std::shared_ptr<HostOpenRegistry> registry,
                 std::string open_key_value, std::uint64_t open_id)
      : path_(std::move(path)),
        access_(access),
        stream_(std::move(stream)),
        registry_(std::move(registry)),
        open_key_(std::move(open_key_value)),
        open_id_(open_id) {}

  ~HostFileHandle() override { release_open(registry_, open_key_, open_id_); }

  [[nodiscard]] FsError read(std::span<std::byte> destination,
                             std::size_t& bytes_read) override {
    std::scoped_lock lock(mutex_);
    const auto error = read_at_locked(position_, destination, bytes_read);
    if (error == FsError::None) position_ += bytes_read;
    return error;
  }

  [[nodiscard]] FsError write(std::span<const std::byte> source,
                              std::size_t& bytes_written) override {
    std::scoped_lock lock(mutex_);
    const auto error = write_at_locked(position_, source, bytes_written);
    if (error == FsError::None) position_ += bytes_written;
    return error;
  }

  [[nodiscard]] FsError read_at(std::uint64_t offset,
                                std::span<std::byte> destination,
                                std::size_t& bytes_read) override {
    std::scoped_lock lock(mutex_);
    return read_at_locked(offset, destination, bytes_read);
  }

  [[nodiscard]] FsError write_at(std::uint64_t offset,
                                 std::span<const std::byte> source,
                                 std::size_t& bytes_written) override {
    std::scoped_lock lock(mutex_);
    return write_at_locked(offset, source, bytes_written);
  }

  [[nodiscard]] FsError seek(std::int64_t offset, SeekOrigin origin,
                             std::uint64_t& new_position) override {
    std::scoped_lock lock(mutex_);
    std::int64_t base = 0;
    if (origin == SeekOrigin::Current) {
      if (position_ > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return FsError::InvalidArgument;
      }
      base = static_cast<std::int64_t>(position_);
    } else if (origin == SeekOrigin::End) {
      std::error_code ec;
      const auto end = std::filesystem::file_size(path_, ec);
      if (ec || end > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return ec ? map_error(ec) : FsError::InvalidArgument;
      }
      base = static_cast<std::int64_t>(end);
    }

    if (offset < 0) {
      const auto magnitude =
          static_cast<std::uint64_t>(-(offset + 1)) + std::uint64_t{1};
      if (magnitude > static_cast<std::uint64_t>(base)) {
        return FsError::InvalidArgument;
      }
    } else if (base > std::numeric_limits<std::int64_t>::max() - offset) {
      return FsError::InvalidArgument;
    }
    const auto candidate = base + offset;
    position_ = static_cast<std::uint64_t>(candidate);
    new_position = position_;
    return FsError::None;
  }

  [[nodiscard]] std::uint64_t tell() const noexcept override {
    std::scoped_lock lock(mutex_);
    return position_;
  }

  [[nodiscard]] FsError size(std::uint64_t& out_size) const override {
    std::scoped_lock lock(mutex_);
    std::error_code ec;
    out_size = std::filesystem::file_size(path_, ec);
    return map_error(ec);
  }

  [[nodiscard]] FsError resize(std::uint64_t new_size) override {
    if (!has_access(access_, FileAccess::Write)) return FsError::AccessDenied;
    std::scoped_lock lock(mutex_);
    stream_.flush();
    std::error_code ec;
    std::filesystem::resize_file(path_, new_size, ec);
    if (ec) return map_error(ec);
    if (position_ > new_size) position_ = new_size;
    return FsError::None;
  }

  [[nodiscard]] FsError flush() override {
    std::scoped_lock lock(mutex_);
    if (!has_access(access_, FileAccess::Write)) return FsError::None;
    stream_.flush();
    return stream_ ? FsError::None : FsError::IoError;
  }

 private:
  [[nodiscard]] FsError read_at_locked(std::uint64_t offset,
                                       std::span<std::byte> destination,
                                       std::size_t& bytes_read) {
    bytes_read = 0;
    if (!has_access(access_, FileAccess::Read)) return FsError::AccessDenied;
    if (destination.empty()) return FsError::None;
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
      return FsError::InvalidArgument;
    }

    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream_) return FsError::IoError;
    stream_.read(reinterpret_cast<char*>(destination.data()),
                 static_cast<std::streamsize>(destination.size()));
    bytes_read = static_cast<std::size_t>(stream_.gcount());
    if (stream_.bad()) return FsError::IoError;
    stream_.clear();
    return FsError::None;
  }

  [[nodiscard]] FsError write_at_locked(std::uint64_t offset,
                                        std::span<const std::byte> source,
                                        std::size_t& bytes_written) {
    bytes_written = 0;
    if (!has_access(access_, FileAccess::Write)) return FsError::AccessDenied;
    if (source.empty()) return FsError::None;
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
      return FsError::InvalidArgument;
    }

    stream_.clear();
    stream_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream_) return FsError::IoError;
    stream_.write(reinterpret_cast<const char*>(source.data()),
                  static_cast<std::streamsize>(source.size()));
    if (!stream_) return FsError::IoError;
    bytes_written = source.size();
    return FsError::None;
  }

  std::filesystem::path path_{};
  FileAccess access_{};
  mutable std::mutex mutex_{};
  std::fstream stream_{};
  std::shared_ptr<HostOpenRegistry> registry_{};
  std::string open_key_{};
  std::uint64_t open_id_{};
  std::uint64_t position_{};
};

#if defined(_WIN32)
// Xbox/NT rename and delete semantics let another handle rename or delete a
// file out from under an already-open handle whenever that handle's own
// ShareAccess grants FileAccess::Delete (KernelIoManager::OpenShareState /
// HostPathDevice::rename() already enforce this in software). The CRT's
// std::fstream cannot express that on Windows: MSVC's file-stream open path
// only ever requests FILE_SHARE_READ/FILE_SHARE_WRITE (the legacy _SH_DENYxx
// share flags predate FILE_SHARE_DELETE), so a file opened via HostFileHandle
// blocks a same-process rename/delete of that same host path even when the
// guest's own share bits allow it - the OS itself denies it underneath the
// software check. Opening with CreateFileW lets the requested dwShareMode
// include FILE_SHARE_DELETE so the host OS enforces exactly what the guest
// asked for, no more and no less.
class HostFileHandleWin32 final : public FileHandle {
 public:
  HostFileHandleWin32(std::filesystem::path path, FileAccess access, HANDLE handle,
                      std::shared_ptr<HostOpenRegistry> registry,
                      std::string open_key_value, std::uint64_t open_id)
      : path_(std::move(path)),
        access_(access),
        handle_(handle),
        registry_(std::move(registry)),
        open_key_(std::move(open_key_value)),
        open_id_(open_id) {}

  ~HostFileHandleWin32() override {
    if (handle_ != INVALID_HANDLE_VALUE) ::CloseHandle(handle_);
    release_open(registry_, open_key_, open_id_);
  }

  [[nodiscard]] FsError read(std::span<std::byte> destination,
                             std::size_t& bytes_read) override {
    std::scoped_lock lock(mutex_);
    const auto error = read_at_locked(position_, destination, bytes_read);
    if (error == FsError::None) position_ += bytes_read;
    return error;
  }

  [[nodiscard]] FsError write(std::span<const std::byte> source,
                              std::size_t& bytes_written) override {
    std::scoped_lock lock(mutex_);
    const auto error = write_at_locked(position_, source, bytes_written);
    if (error == FsError::None) position_ += bytes_written;
    return error;
  }

  [[nodiscard]] FsError read_at(std::uint64_t offset,
                                std::span<std::byte> destination,
                                std::size_t& bytes_read) override {
    std::scoped_lock lock(mutex_);
    return read_at_locked(offset, destination, bytes_read);
  }

  [[nodiscard]] FsError write_at(std::uint64_t offset,
                                 std::span<const std::byte> source,
                                 std::size_t& bytes_written) override {
    std::scoped_lock lock(mutex_);
    return write_at_locked(offset, source, bytes_written);
  }

  [[nodiscard]] FsError seek(std::int64_t offset, SeekOrigin origin,
                             std::uint64_t& new_position) override {
    std::scoped_lock lock(mutex_);
    std::int64_t base = 0;
    if (origin == SeekOrigin::Current) {
      if (position_ > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return FsError::InvalidArgument;
      }
      base = static_cast<std::int64_t>(position_);
    } else if (origin == SeekOrigin::End) {
      LARGE_INTEGER end{};
      if (!::GetFileSizeEx(handle_, &end)) return map_win32_error(::GetLastError());
      base = end.QuadPart;
    }

    if (offset < 0) {
      const auto magnitude =
          static_cast<std::uint64_t>(-(offset + 1)) + std::uint64_t{1};
      if (magnitude > static_cast<std::uint64_t>(base)) {
        return FsError::InvalidArgument;
      }
    } else if (base > std::numeric_limits<std::int64_t>::max() - offset) {
      return FsError::InvalidArgument;
    }
    const auto candidate = base + offset;
    position_ = static_cast<std::uint64_t>(candidate);
    new_position = position_;
    return FsError::None;
  }

  [[nodiscard]] std::uint64_t tell() const noexcept override {
    std::scoped_lock lock(mutex_);
    return position_;
  }

  [[nodiscard]] FsError size(std::uint64_t& out_size) const override {
    std::scoped_lock lock(mutex_);
    LARGE_INTEGER end{};
    if (!::GetFileSizeEx(handle_, &end)) return map_win32_error(::GetLastError());
    out_size = static_cast<std::uint64_t>(end.QuadPart);
    return FsError::None;
  }

  [[nodiscard]] FsError resize(std::uint64_t new_size) override {
    if (!has_access(access_, FileAccess::Write)) return FsError::AccessDenied;
    std::scoped_lock lock(mutex_);
    LARGE_INTEGER distance{};
    distance.QuadPart = static_cast<LONGLONG>(new_size);
    if (!::SetFilePointerEx(handle_, distance, nullptr, FILE_BEGIN)) {
      return map_win32_error(::GetLastError());
    }
    if (!::SetEndOfFile(handle_)) return map_win32_error(::GetLastError());
    if (position_ > new_size) position_ = new_size;
    return FsError::None;
  }

  [[nodiscard]] FsError flush() override {
    if (!has_access(access_, FileAccess::Write)) return FsError::None;
    std::scoped_lock lock(mutex_);
    return ::FlushFileBuffers(handle_) ? FsError::None : map_win32_error(::GetLastError());
  }

 private:
  [[nodiscard]] static FsError map_win32_error(DWORD win32_error) noexcept {
    return map_error(std::error_code(static_cast<int>(win32_error), std::system_category()));
  }

  [[nodiscard]] FsError read_at_locked(std::uint64_t offset,
                                       std::span<std::byte> destination,
                                       std::size_t& bytes_read) {
    bytes_read = 0;
    if (!has_access(access_, FileAccess::Read)) return FsError::AccessDenied;
    if (destination.empty()) return FsError::None;

    LARGE_INTEGER distance{};
    distance.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(handle_, distance, nullptr, FILE_BEGIN)) {
      return map_win32_error(::GetLastError());
    }
    DWORD read = 0;
    if (!::ReadFile(handle_, destination.data(),
                    static_cast<DWORD>(destination.size()), &read, nullptr)) {
      return map_win32_error(::GetLastError());
    }
    bytes_read = read;
    return FsError::None;
  }

  [[nodiscard]] FsError write_at_locked(std::uint64_t offset,
                                        std::span<const std::byte> source,
                                        std::size_t& bytes_written) {
    bytes_written = 0;
    if (!has_access(access_, FileAccess::Write)) return FsError::AccessDenied;
    if (source.empty()) return FsError::None;

    LARGE_INTEGER distance{};
    distance.QuadPart = static_cast<LONGLONG>(offset);
    if (!::SetFilePointerEx(handle_, distance, nullptr, FILE_BEGIN)) {
      return map_win32_error(::GetLastError());
    }
    DWORD written = 0;
    if (!::WriteFile(handle_, source.data(), static_cast<DWORD>(source.size()),
                     &written, nullptr)) {
      return map_win32_error(::GetLastError());
    }
    bytes_written = written;
    return FsError::None;
  }

  std::filesystem::path path_{};
  FileAccess access_{};
  mutable std::mutex mutex_{};
  HANDLE handle_{INVALID_HANDLE_VALUE};
  std::shared_ptr<HostOpenRegistry> registry_{};
  std::string open_key_{};
  std::uint64_t open_id_{};
  std::uint64_t position_{};
};
#endif  // defined(_WIN32)

}  // namespace xenon::filesystem
