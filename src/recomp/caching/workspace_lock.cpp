#include "xenon/recomp/workspace_lock.hpp"

#include <chrono>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>

#include "xenon/core/json.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <signal.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/time.h>
#endif
#endif

namespace xenon::recomp {
namespace {

#if defined(_WIN32)
std::uint64_t filetime_value(const FILETIME& time) {
  return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32u) | time.dwLowDateTime;
}

std::uint64_t process_creation_time(HANDLE process) {
  FILETIME creation{}, exit{}, kernel{}, user{};
  if (!::GetProcessTimes(process, &creation, &exit, &kernel, &user)) return 0;
  return filetime_value(creation);
}
#elif defined(__linux__)
// Field 22 of /proc/<pid>/stat: start time in clock ticks since boot.
std::uint64_t linux_start_time(std::uint64_t pid) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto close = text.rfind(')');  // the command name may contain spaces
  if (close == std::string::npos) return 0;
  std::istringstream fields(text.substr(close + 2));
  std::string field;
  for (int i = 3; i <= 22 && fields >> field; ++i) {
    if (i == 22) return std::stoull(field);
  }
  return 0;
}
#endif

std::optional<ProcessIdentity> read_owner(const std::filesystem::path& workspace) {
  std::ifstream in(WorkspaceLock::owner_path(workspace), std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream text;
  text << in.rdbuf();
  core::JsonValue owner;
  if (!core::JsonValue::parse(text.str(), owner) || !owner.is_object()) return std::nullopt;
  // Large integers are written as strings: a JSON number loses precision
  // past 2^53.
  try {
    ProcessIdentity identity{};
    identity.pid = std::stoull(owner.get_string("pid"));
    identity.start_time = std::stoull(owner.get_string("startTime", "0"));
    return identity;
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

void write_owner(const std::filesystem::path& workspace, const ProcessIdentity& identity) {
  auto owner = core::JsonValue::make_object();
  owner.set("pid", std::to_string(identity.pid));
  owner.set("startTime", std::to_string(identity.start_time));
  std::ofstream out(WorkspaceLock::owner_path(workspace), std::ios::binary | std::ios::trunc);
  out << owner.dump() << "\n";
}

// Empty when the lock is held; otherwise why it is abandoned.
std::optional<std::string> abandoned_reason(const std::filesystem::path& workspace) {
  if (const auto owner = read_owner(workspace)) {
    if (process_is_running(*owner)) return std::nullopt;
    return "its owner (process " + std::to_string(owner->pid) + ") is no longer running";
  }
  std::error_code ec;
  const auto created = std::filesystem::last_write_time(WorkspaceLock::lock_path(workspace), ec);
  const auto boot = host_boot_time();
  if (!ec && boot && created < *boot) return "it has no owner and predates the last boot";
  return std::nullopt;
}

std::string holder_description(const std::filesystem::path& workspace) {
  if (const auto owner = read_owner(workspace))
    return "held by running process " + std::to_string(owner->pid);
  return "held by a process that has not recorded itself yet";
}

}  // namespace

ProcessIdentity current_process_identity() {
#if defined(_WIN32)
  return {::GetCurrentProcessId(), process_creation_time(::GetCurrentProcess())};
#elif defined(__linux__)
  const auto pid = static_cast<std::uint64_t>(::getpid());
  return {pid, linux_start_time(pid)};
#else
  return {static_cast<std::uint64_t>(::getpid()), 0};
#endif
}

bool process_is_running(const ProcessIdentity& process) {
  if (process.pid == 0) return false;
#if defined(_WIN32)
  HANDLE handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                static_cast<DWORD>(process.pid));
  if (!handle) {
    // A process we may not open (another user's) still exists.
    return ::GetLastError() == ERROR_ACCESS_DENIED;
  }
  DWORD exit_code = 0;
  const bool active = ::GetExitCodeProcess(handle, &exit_code) && exit_code == STILL_ACTIVE;
  const auto start = process_creation_time(handle);
  ::CloseHandle(handle);
  if (!active) return false;
  return process.start_time == 0 || start == 0 || start == process.start_time;
#else
  if (::kill(static_cast<pid_t>(process.pid), 0) != 0 && errno != EPERM) return false;
#if defined(__linux__)
  if (process.start_time != 0) {
    const auto start = linux_start_time(process.pid);
    if (start != 0 && start != process.start_time) return false;
  }
#endif
  return true;
#endif
}

std::optional<std::filesystem::file_time_type> host_boot_time() {
  using namespace std::chrono;
  std::uint64_t uptime_ms = 0;
#if defined(_WIN32)
  uptime_ms = ::GetTickCount64();
#elif defined(__linux__)
  std::ifstream in("/proc/uptime");
  double seconds = 0.0;
  if (!(in >> seconds)) return std::nullopt;
  uptime_ms = static_cast<std::uint64_t>(seconds * 1000.0);
#elif defined(__APPLE__)
  timeval boot{};
  std::size_t size = sizeof(boot);
  int mib[2] = {CTL_KERN, KERN_BOOTTIME};
  if (::sysctl(mib, 2, &boot, &size, nullptr, 0) != 0) return std::nullopt;
  timeval now{};
  ::gettimeofday(&now, nullptr);
  uptime_ms = static_cast<std::uint64_t>(now.tv_sec - boot.tv_sec) * 1000u;
#else
  return std::nullopt;
#endif
  return std::filesystem::file_time_type::clock::now() -
         duration_cast<std::filesystem::file_time_type::duration>(milliseconds(uptime_ms));
}

WorkspaceLock::WorkspaceLock(std::filesystem::path workspace) : workspace_(std::move(workspace)) {
  std::filesystem::create_directories(workspace_);
  const auto lock = lock_path(workspace_);
  // Two attempts: the second follows taking over an abandoned lock.
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (std::filesystem::create_directory(lock)) {
      write_owner(workspace_, current_process_identity());
      return;
    }
    const auto reason = abandoned_reason(workspace_);
    if (!reason) break;
    // Rename first: only one of several processes taking over the same
    // abandoned lock succeeds; the others see the new lock on retry.
    const auto stale = workspace_ / (".lock.abandoned." + std::to_string(current_process_identity().pid));
    std::error_code ec;
    std::filesystem::remove_all(stale, ec);
    std::filesystem::rename(lock, stale, ec);
    if (!ec) {
      std::filesystem::remove_all(stale, ec);
      reclaimed_ = true;
    }
  }
  throw std::runtime_error("preparation workspace is busy (" + holder_description(workspace_) +
                           "): " + workspace_.string());
}

WorkspaceLock::~WorkspaceLock() {
  std::error_code ec;
  std::filesystem::remove_all(lock_path(workspace_), ec);
}

}  // namespace xenon::recomp
