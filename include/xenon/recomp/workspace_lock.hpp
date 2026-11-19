#pragma once

// Exclusive ownership of a preparation workspace directory, held by one
// process at a time and safe against that process dying without releasing
// it (a crash, a kill, a power cut).
//
// The lock is the directory <workspace>/.lock, created atomically, holding
// owner.json with the owning process's ID and start time. A lock is
// abandoned, and is taken over, when:
//   - its owner process no longer exists, or the ID now belongs to a
//     different process (its start time differs); or
//   - it has no owner record (written by an older Xenon, or its owner died
//     between creating the directory and writing the record) and it was
//     created before the host last booted, which no process survives.
// A lock without an owner record created since boot is treated as held: it
// may belong to a process that is just acquiring it.
// Take-over renames the abandoned lock away first, so of two processes
// finding the same abandoned lock only one can win it.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace xenon::recomp {

struct ProcessIdentity {
  std::uint64_t pid{};
  // Host-specific start time of the process (Windows FILETIME of creation,
  // Linux clock ticks since boot); 0 when the host cannot report one, and
  // then only the ID is compared.
  std::uint64_t start_time{};
  friend bool operator==(const ProcessIdentity&, const ProcessIdentity&) = default;
};

[[nodiscard]] ProcessIdentity current_process_identity();
// Whether that exact process (same ID and, when known, same start time) is
// still running.
[[nodiscard]] bool process_is_running(const ProcessIdentity& process);
// When the host last booted, or empty when it cannot be determined.
[[nodiscard]] std::optional<std::filesystem::file_time_type> host_boot_time();

class WorkspaceLock {
 public:
  // Takes the lock on `workspace` (created if missing). Throws
  // std::runtime_error naming the owner when another live process holds it.
  explicit WorkspaceLock(std::filesystem::path workspace);
  ~WorkspaceLock();
  WorkspaceLock(const WorkspaceLock&) = delete;
  WorkspaceLock& operator=(const WorkspaceLock&) = delete;

  [[nodiscard]] const std::filesystem::path& workspace() const noexcept { return workspace_; }
  // True when an abandoned lock was taken over to acquire this one.
  [[nodiscard]] bool reclaimed() const noexcept { return reclaimed_; }

  [[nodiscard]] static std::filesystem::path lock_path(const std::filesystem::path& workspace) {
    return workspace / ".lock";
  }
  [[nodiscard]] static std::filesystem::path owner_path(const std::filesystem::path& workspace) {
    return lock_path(workspace) / "owner.json";
  }

 private:
  std::filesystem::path workspace_{};
  bool reclaimed_{};
};

}  // namespace xenon::recomp
