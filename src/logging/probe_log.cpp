#include "xenon/logging/probe_log.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#if defined(_WIN32)
#include <share.h>
#endif

namespace xenon::logging {
namespace {

bool enabled_from_environment() {
  const char* value = std::getenv("XENON_PROBE_LOGS");
  return value != nullptr && *value != '\0' && std::string_view(value) != "0";
}

std::atomic<bool> g_enabled{enabled_from_environment()};

// One probe file of this run: kept open from its first record (which
// truncates it) until the run's directory changes, its cap is reached or
// probe logging is turned off (then it reopens for appending on the next
// record, so turning logging off releases the files without losing them).
// Opening, appending to and closing the file for every record - under the
// one lock all probe writers share - made each record cost a file open and
// close (more under on-access antivirus scanning), and every thread logging
// queued behind them: AC6's main thread spent 63 ms in an NtResumeThread
// whose resume logged a line, long enough for the thread it resumed to
// reach a critical section the main thread had not initialized yet.
struct ProbeFile {
  std::FILE* file{};
  std::uint64_t written{};
};

// Files written this run, by name. A file absent from the map has not been
// written yet in this run and is truncated by its first record.
std::mutex g_mutex;
std::unordered_map<std::string, ProbeFile> g_files;
std::string g_directory;

void close_all_locked() {
  for (auto& [name, probe] : g_files)
    if (probe.file) std::fclose(probe.file);
  g_files.clear();
}

// Opened shareable, so the file can be read (by tests, or while a session
// still runs) while it stays open here.
std::FILE* open_shared(const std::filesystem::path& path, bool truncate) {
#if defined(_WIN32)
  return _wfsopen(path.c_str(), truncate ? L"wb" : L"ab", _SH_DENYNO);
#else
  return std::fopen(path.c_str(), truncate ? "wb" : "ab");
#endif
}

}  // namespace

bool probe_logs_enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }

void set_probe_logs_enabled(bool enabled) noexcept {
  g_enabled.store(enabled, std::memory_order_relaxed);
  if (enabled) return;
  // Release the open files (their directory may be removed next) but keep
  // what was written, so a later record appends rather than truncating.
  std::scoped_lock lock(g_mutex);
  for (auto& [name, probe] : g_files) {
    if (probe.file) std::fclose(probe.file);
    probe.file = nullptr;
  }
}

void set_probe_log_directory(const std::string& directory) {
  std::scoped_lock lock(g_mutex);
  if (directory == g_directory) return;
  if (!directory.empty()) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
  }
  g_directory = directory;
  close_all_locked();
}

std::string probe_log_directory() {
  std::scoped_lock lock(g_mutex);
  return g_directory;
}

void append_probe_log(const char* file_name, const char* format, ...) {
  if (!probe_logs_enabled()) return;
  char record[1024];
  va_list arguments;
  va_start(arguments, format);
  const int formatted = std::vsnprintf(record, sizeof(record), format, arguments);
  va_end(arguments);
  if (formatted < 0) return;
  const auto length =
      std::min<std::size_t>(static_cast<std::size_t>(formatted), sizeof(record) - 1);

  std::scoped_lock lock(g_mutex);
  const auto [entry, first] = g_files.try_emplace(file_name);
  auto& probe = entry->second;
  if (probe.written >= kProbeLogByteLimit) return;
  if (!probe.file) {
    // Binary mode keeps the file the size the cap counts; Windows text mode
    // would add a carriage return before every newline. The first record
    // truncates; a file released while logging was off is appended to.
    const auto path = g_directory.empty() ? std::filesystem::path(file_name)
                                          : std::filesystem::path(g_directory) / file_name;
    probe.file = open_shared(path, first);
  }
  if (!probe.file) {
    // Unopenable: retried by the next record (which truncates it if this
    // run never wrote it).
    if (first) g_files.erase(entry);
    return;
  }
  if (probe.written + length > kProbeLogByteLimit) {
    std::fputs("[probe log truncated: per-run limit reached]\n", probe.file);
    probe.written = kProbeLogByteLimit;
    std::fclose(probe.file);
    probe.file = nullptr;
    return;
  }
  std::fwrite(record, 1, length, probe.file);
  probe.written += length;
  // Each record reaches the file as it is written, as before: a probe's last
  // lines are the ones a crash or hang investigation needs.
  std::fflush(probe.file);
}

}  // namespace xenon::logging
