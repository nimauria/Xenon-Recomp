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

bool Logger::set_error_directory(const std::string& directory, Level min_level) {
  std::scoped_lock lock(error_file_mutex_);
  error_files_.clear();
  error_directory_.clear();
  if (directory.empty()) return true;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error || !std::filesystem::is_directory(directory, error)) return false;
  error_directory_ = directory;
  error_file_level_ = min_level;
  return true;
}

void Logger::log(Level level, std::string_view category, std::string_view message) {
  call_count_.fetch_add(1, std::memory_order_relaxed);

  {
    std::scoped_lock lock(error_file_mutex_);
    std::ofstream* file = nullptr;
    if (!error_directory_.empty() && level >= error_file_level_) {
      auto found = error_files_.find(category);
      if (found == error_files_.end()) {
        const auto path = std::filesystem::path(error_directory_) / error_file_name(category);
        auto opened = std::make_unique<std::ofstream>(path, std::ios::out | std::ios::app);
        found = error_files_.emplace(std::string(category), std::move(opened)).first;
      }
      if (*found->second) file = found->second.get();
    }
    if (file) {
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
      *file << stamp << ' ' << level_name(level) << ": " << message << '\n';
      file->flush();
      error_file_lines_.fetch_add(1, std::memory_order_relaxed);
    }
  }

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
