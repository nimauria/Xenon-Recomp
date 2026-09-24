#include "xenon/logging/logger.hpp"

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

void Logger::log(Level level, std::string_view category, std::string_view message) {
  call_count_.fetch_add(1, std::memory_order_relaxed);

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
  stream << entry.dump() << '\n';
}

}  // namespace xenon::logging
