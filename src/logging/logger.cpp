#include "xenon/logging/logger.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iostream>

#include "xenon/core/json.hpp"

namespace xenon::logging {
namespace {

std::string_view level_name(Level level) {
  switch (level) {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warning: return "warning";
    case Level::Error: return "error";
    case Level::Fatal: return "fatal";
  }
  return "unknown";
}

}  // namespace

Logger& Logger::instance() {
  static Logger logger;
  return logger;
}

void Logger::set_sink(Sink sink) {
  std::scoped_lock lock(sink_mutex_);
  sink_ = std::move(sink);
}

std::string Logger::error_file_name(std::string_view category) {
  std::string name;
  for (const char c : category) {
    const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '-' || c == '_';
    name += keep ? c : '_';
  }
  if (name.empty()) name = "general";
  return name + ".log";
}

bool Logger::set_error_directory(const std::string& directory, Level min_level,
                                 std::uint64_t file_byte_limit) {
  std::scoped_lock lock(error_file_mutex_);
  error_files_.clear();
  error_repeats_.clear();
  error_directory_.clear();
  if (directory.empty()) return true;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error || !std::filesystem::is_directory(directory, error)) return false;
  error_directory_ = directory;
  error_file_level_ = min_level;
  error_file_byte_limit_ = file_byte_limit;
  return true;
}

std::string Logger::error_directory() const {
  std::scoped_lock lock(error_file_mutex_);
  return error_directory_;
}

void Logger::record(Level level, std::string_view category, std::string_view message) {
  write_error_file(level, category, message);
}

void Logger::write_error_file(Level level, std::string_view category, std::string_view message) {
  std::scoped_lock lock(error_file_mutex_);
  if (error_directory_.empty() || level < error_file_level_) return;

  std::string key;
  key.reserve(category.size() + message.size() + 4u);
  key.append(level_name(level)).append(1, '\n').append(category).append(1, '\n').append(message);
  // Past this many distinct entries new ones are written without being
  // counted, so a run of unique messages cannot grow the table without bound
  // (the file size limit still applies).
  constexpr std::size_t kMaxCountedEntries = 1u << 16;
  std::uint64_t occurrence = 1u;
  if (const auto counted = error_repeats_.find(key); counted != error_repeats_.end()) {
    occurrence = ++counted->second;
  } else if (error_repeats_.size() < kMaxCountedEntries) {
    error_repeats_.emplace(std::move(key), 1u);
  }
  std::string repeated;
  if (occurrence > kRepeatsWritten) {
    if ((occurrence - kRepeatsWritten) % kRepeatInterval != 0u) return;
    repeated = " (repeated " + std::to_string(occurrence) + " times)";
  }

  auto found = error_files_.find(category);
  if (found == error_files_.end()) {
    const auto path = std::filesystem::path(error_directory_) / error_file_name(category);
    auto opened = std::make_unique<ErrorFile>();
    opened->stream.open(path, std::ios::out | std::ios::app);
    found = error_files_.emplace(std::string(category), std::move(opened)).first;
  }
  auto& file = *found->second;
  if (!file.stream || file.bytes >= error_file_byte_limit_) return;

  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::system_clock::to_time_t(now);
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                          now.time_since_epoch()).count() % 1000;
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  char stamp[32];
  std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
                utc.tm_sec, static_cast<int>(millis));
  std::string line = std::string(stamp) + ' ' + std::string(level_name(level)) + ": ";
  line.append(message).append(repeated).append(1, '\n');
  if (file.bytes + line.size() > error_file_byte_limit_) {
    line = std::string(stamp) + " error: this file reached its size limit; later entries are dropped\n";
    file.bytes = error_file_byte_limit_;
  } else {
    file.bytes += line.size();
  }
  file.stream << line;
  file.stream.flush();
  error_file_lines_.fetch_add(1, std::memory_order_relaxed);
}

void Logger::log(Level level, std::string_view category, std::string_view message) {
  call_count_.fetch_add(1, std::memory_order_relaxed);
  write_error_file(level, category, message);

  Sink sink_copy;
  {
    std::scoped_lock lock(sink_mutex_);
    sink_copy = sink_;
  }
  if (sink_copy) {
    sink_copy(level, category, message);
    return;
  }

  core::JsonValue entry = core::JsonValue::make_object();
  entry.set("level", std::string(level_name(level)));
  entry.set("category", std::string(category));
  entry.set("message", std::string(message));
  auto& stream = (level >= Level::Error) ? std::cerr : std::cout;
  std::scoped_lock lock(stream_mutex_);
  stream << entry.dump() << '\n';
}

}  // namespace xenon::logging
