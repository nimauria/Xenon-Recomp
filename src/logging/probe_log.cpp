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

namespace xenon::logging {
namespace {

bool enabled_from_environment() {
  const char* value = std::getenv("XENON_PROBE_LOGS");
  return value != nullptr && *value != '\0' && std::string_view(value) != "0";
}

std::atomic<bool> g_enabled{enabled_from_environment()};

// Bytes written this run, per file. A file absent from the map has not been
// written yet in this process and is truncated by its first record.
std::mutex g_mutex;
std::unordered_map<std::string, std::uint64_t> g_written;
std::string g_directory;

}  // namespace

bool probe_logs_enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }

void set_probe_logs_enabled(bool enabled) noexcept {
  g_enabled.store(enabled, std::memory_order_relaxed);
}

void set_probe_log_directory(const std::string& directory) {
  std::scoped_lock lock(g_mutex);
  if (directory == g_directory) return;
  if (!directory.empty()) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
  }
  g_directory = directory;
  g_written.clear();
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
  const auto [entry, first] = g_written.try_emplace(file_name, 0u);
  auto& written = entry->second;
  if (written >= kProbeLogByteLimit) return;
  // Binary mode keeps the file the size the cap counts; Windows text mode
  // would add a carriage return before every newline.
  const auto path = g_directory.empty()
                        ? std::filesystem::path(file_name)
                        : std::filesystem::path(g_directory) / file_name;
#if defined(_WIN32)
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), first ? L"wb" : L"ab") != 0) file = nullptr;
#else
  FILE* file = std::fopen(path.c_str(), first ? "wb" : "ab");
#endif
  if (!file) return;
  if (written + length > kProbeLogByteLimit) {
    std::fputs("[probe log truncated: per-run limit reached]\n", file);
    written = kProbeLogByteLimit;
  } else {
    std::fwrite(record, 1, length, file);
    written += length;
  }
  std::fclose(file);
}

}  // namespace xenon::logging
