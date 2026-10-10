#include "xenon/logging/probe_log.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
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

}  // namespace

bool probe_logs_enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }

void set_probe_logs_enabled(bool enabled) noexcept {
  g_enabled.store(enabled, std::memory_order_relaxed);
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
  FILE* file = std::fopen(file_name, first ? "w" : "a");
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
