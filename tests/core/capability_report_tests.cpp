// Phase 0 of the AC6 Runtime Readiness / Platform Fidelity pass:
// CapabilityReportBuilder aggregation and the Logger's cheap-when-disabled
// gate, plus a smoke test that a real XenonSession can produce a report end
// to end.

#include "xenon/core/capability_report.hpp"
#include "xenon/core/session.hpp"
#include "xenon/logging/logger.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using xenon::core::CapabilityReportBuilder;
using xenon::core::JsonValue;
using xenon::core::RunFingerprint;
using xenon::logging::Level;
using xenon::logging::Logger;

namespace {

void test_builder_aggregates_sections_deterministically() {
  CapabilityReportBuilder builder;
  assert(!builder.has_section("imports"));

  JsonValue imports = JsonValue::make_object();
  imports.set("implemented", 10);
  imports.set("missing", 0);
  builder.set_section("imports", imports);

  JsonValue fallback = JsonValue::make_object();
  fallback.set("aotBlocks", 100);
  fallback.set("fallbackBlocks", 3);
  builder.set_section("fallback", fallback);

  assert(builder.has_section("imports"));
  assert(builder.has_section("fallback"));
  assert(!builder.has_section("gpu"));

  const JsonValue* found = builder.find_section("imports");
  assert(found != nullptr);
  assert(found->get_number("implemented") == 10.0);

  // Insertion order must not affect the serialized document: sections is a
  // JsonValue::Object (std::map), so two builders populated in different
  // orders must dump identically.
  CapabilityReportBuilder reordered;
  reordered.set_section("fallback", fallback);
  reordered.set_section("imports", imports);

  const std::string first = builder.build().dump();
  const std::string second = reordered.build().dump();
  assert(first == second);

  // Replacing a section (not appending a duplicate) is idempotent.
  builder.set_section("imports", imports);
  assert(builder.build().dump() == first);
}

void test_report_json_round_trips() {
  CapabilityReportBuilder builder;
  RunFingerprint fingerprint{};
  fingerprint.effective_xex_sha1 = "deadbeef";
  fingerprint.gpu_backend = "vulkan";
  fingerprint.host_os = "windows";
  builder.set_run_fingerprint(fingerprint);

  const JsonValue report = builder.build();
  const std::string dumped = report.dump();

  JsonValue parsed;
  std::string error;
  assert(JsonValue::parse(dumped, parsed, &error));
  assert(error.empty());

  const JsonValue* sections = parsed.find("sections");
  assert(sections != nullptr);
  const JsonValue* fp = sections->find("runFingerprint");
  assert(fp != nullptr);
  assert(fp->get_string("effectiveXexSha1") == "deadbeef");
  assert(fp->get_string("gpuBackend") == "vulkan");
  assert(fp->get_string("hostOs") == "windows");

  // Clearing removes everything, including the fingerprint section.
  builder.clear();
  assert(!builder.has_section("runFingerprint"));
  // Bind build()'s result to a named local before calling find() on it: the
  // JsonValue it returns is a temporary, and a pointer taken from a
  // temporary's find() dangles the moment the full expression ends.
  const JsonValue empty_report = builder.build();
  const JsonValue* empty_sections = empty_report.find("sections");
  assert(empty_sections != nullptr && empty_sections->as_object()->empty());
}

void test_logger_disabled_level_does_no_work() {
  Logger& logger = Logger::instance();
  logger.set_sink(nullptr);  // restore default sink for isolation
  logger.set_min_level(Level::Warning);

  const std::uint64_t before = logger.call_count();
  bool message_built = false;
  logger.log_if_enabled(Level::Debug, "test", [&] {
    message_built = true;
    return std::string("this should never be constructed");
  });
  assert(!message_built && "message builder must not run below the min level");
  assert(logger.call_count() == before && "call_count must not change for a disabled level");

  logger.log_if_enabled(Level::Error, "test", [&] {
    message_built = true;
    return std::string("this should run");
  });
  assert(message_built);
  assert(logger.call_count() == before + 1);
}

void test_logger_sink_receives_enabled_entries() {
  Logger& logger = Logger::instance();
  logger.set_min_level(Level::Trace);

  struct Captured {
    Level level;
    std::string category;
    std::string message;
  };
  std::vector<Captured> captured;
  logger.set_sink([&](Level level, std::string_view category, std::string_view message) {
    captured.push_back(Captured{level, std::string(category), std::string(message)});
  });

  logger.log(Level::Info, "capability", "hello");
  assert(captured.size() == 1);
  assert(captured[0].level == Level::Info);
  assert(captured[0].category == "capability");
  assert(captured[0].message == "hello");

  logger.set_sink(nullptr);
  logger.set_min_level(Level::Warning);
}

void test_session_produces_capability_report() {
  xenon::core::XenonSession session;
  xenon::core::SessionConfig config{};
  config.enable_logging = false;
  config.enable_graphics = false;
  config.enable_input = false;
  config.enable_audio = false;
  const bool ready = session.initialize(config).success;
  assert(ready && "a minimally-configured session must still initialize");

  const JsonValue report = session.capability_report();
  const JsonValue* sections = report.find("sections");
  assert(sections != nullptr);
  const JsonValue* fingerprint = sections->find("runFingerprint");
  assert(fingerprint != nullptr);
  // No title loaded yet: identity fields are empty, not garbage/absent.
  assert(fingerprint->get_string("effectiveXexSha1").empty());
  assert(fingerprint->get_string("tuIdentity") == "none");
  assert(!fingerprint->get_string("hostOs").empty());
  assert(!fingerprint->get_string("hostCpuArch").empty());

  // Two independently-initialized sessions with identical config produce
  // identical fingerprints (determinism the differential-checkpoint and
  // multi-run comparison work in later phases depends on).
  xenon::core::XenonSession other_session;
  assert(other_session.initialize(config).success);
  const JsonValue other_report = other_session.capability_report();
  const JsonValue* other_fingerprint = other_report.find("sections")->find("runFingerprint");
  assert(other_fingerprint != nullptr);
  assert(fingerprint->dump() == other_fingerprint->dump());

  session.shutdown();
  other_session.shutdown();
}

}  // namespace

int main() {
  std::cout << "Testing CapabilityReportBuilder / Logger...\n" << std::flush;

  std::cout << "  builder_aggregates...\n" << std::flush;
  test_builder_aggregates_sections_deterministically();
  std::cout << "  report_json_round_trips...\n" << std::flush;
  test_report_json_round_trips();
  std::cout << "  logger_disabled...\n" << std::flush;
  test_logger_disabled_level_does_no_work();
  std::cout << "  logger_sink...\n" << std::flush;
  test_logger_sink_receives_enabled_entries();
  std::cout << "  session_produces_report...\n" << std::flush;
  test_session_produces_capability_report();

  std::cout << "All capability report tests passed!\n";
  return 0;
}
