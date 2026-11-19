#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "xenon/logging/logger.hpp"

using xenon::logging::Level;
using xenon::logging::Logger;

namespace {

std::vector<std::string> read_lines(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::vector<std::string> lines;
  for (std::string line; std::getline(in, line);) lines.push_back(line);
  return lines;
}

}  // namespace

int main() {
  auto& logger = Logger::instance();
  // Keep the default stdout/stderr writer quiet: these tests only read files.
  logger.set_sink([](Level, std::string_view, std::string_view) {});

  const auto root = std::filesystem::temp_directory_path() / "xenon_logger_error_files_test";
  std::filesystem::remove_all(root);
  const auto directory = root / "errors" / "2026-10-30_20-15-02";

  // Every category gets its own file; only entries at the minimum level or
  // above are written; each line carries a UTC timestamp and the level.
  assert(logger.set_error_directory(directory.string(), Level::Warning));
  const auto before = logger.error_file_lines();
  logger.log(Level::Error, "gpu", "draw failed: pixel shader 0x1234 did not compile");
  logger.log(Level::Warning, "filesystem", "game:\\missing.bin was not found");
  logger.log(Level::Info, "gpu", "routine message, below the minimum");
  logger.log(Level::Fatal, "gpu", "device removed");
  logger.log(Level::Error, "", "no category");
  logger.log(Level::Error, "xam/content", "odd characters in a category");
  assert(logger.error_file_lines() - before == 5u);

  const auto gpu = read_lines(directory / "gpu.log");
  assert(gpu.size() == 2u);
  assert(gpu[0].find(" error: draw failed: pixel shader 0x1234 did not compile") != std::string::npos);
  assert(gpu[1].find(" fatal: device removed") != std::string::npos);
  // "2026-10-30T20:15:02.123Z error: ..." - an ISO-8601 UTC stamp first.
  assert(gpu[0].size() > 24u && gpu[0][4] == '-' && gpu[0][10] == 'T' && gpu[0][23] == 'Z');

  const auto filesystem = read_lines(directory / "filesystem.log");
  assert(filesystem.size() == 1u && filesystem[0].find(" warning: game:\\missing.bin") != std::string::npos);
  assert(read_lines(directory / "general.log").size() == 1u);
  assert(Logger::error_file_name("xam/content") == "xam_content.log");
  assert(read_lines(directory / "xam_content.log").size() == 1u);

  // Closing stops the writes; reopening appends rather than truncating.
  assert(logger.set_error_directory(""));
  logger.log(Level::Error, "gpu", "after closing");
  assert(read_lines(directory / "gpu.log").size() == 2u);
  assert(logger.set_error_directory(directory.string(), Level::Error));
  logger.log(Level::Warning, "gpu", "below the new minimum");
  logger.log(Level::Error, "gpu", "appended");
  assert(read_lines(directory / "gpu.log").size() == 3u);

  // A directory that cannot be created leaves nothing open.
  std::ofstream(root / "a_file") << "x";
  assert(!logger.set_error_directory((root / "a_file" / "sub").string()));
  logger.log(Level::Error, "gpu", "not written anywhere");
  assert(read_lines(directory / "gpu.log").size() == 3u);

  // record() writes the file only: the sink (and so the console) never sees
  // it, and it shares the level filter.
  const auto recorded = root / "errors" / "2026-10-30_21-00-00";
  assert(logger.set_error_directory(recorded.string()));
  assert(logger.error_directory() == recorded.string());
  int sink_calls = 0;
  logger.set_sink([&](Level, std::string_view, std::string_view) { ++sink_calls; });
  logger.record(Level::Error, "guest", "trap 0 at 0x82001234");
  logger.record(Level::Info, "guest", "below the minimum");
  assert(sink_calls == 0);
  auto guest = read_lines(recorded / "guest.log");
  assert(guest.size() == 1u && guest[0].find(" error: trap 0 at 0x82001234") != std::string::npos);
  logger.log(Level::Error, "guest", "through log()");
  assert(sink_calls == 1 && read_lines(recorded / "guest.log").size() == 2u);

  // A repeated entry is written kRepeatsWritten times, then once per
  // kRepeatInterval more, saying how many there have been. A different
  // message, level or category counts separately.
  const auto repeats = Logger::kRepeatsWritten + 2u * Logger::kRepeatInterval;
  for (std::uint64_t i = 0; i < repeats; ++i) {
    logger.record(Level::Warning, "audio", "underrun");
  }
  logger.record(Level::Error, "audio", "underrun");
  logger.record(Level::Warning, "audio", "a different problem");
  const auto audio = read_lines(recorded / "audio.log");
  assert(audio.size() == Logger::kRepeatsWritten + 2u + 2u);
  const auto first_summary = audio[Logger::kRepeatsWritten];
  assert(first_summary.find(" warning: underrun (repeated " +
                            std::to_string(Logger::kRepeatsWritten + Logger::kRepeatInterval) +
                            " times)") != std::string::npos);
  assert(audio[Logger::kRepeatsWritten + 1u].find(
             "(repeated " + std::to_string(repeats) + " times)") != std::string::npos);
  assert(audio[Logger::kRepeatsWritten + 2u].find(" error: underrun") != std::string::npos);
  assert(audio.back().find(" warning: a different problem") != std::string::npos);
  // A new run starts counting again.
  assert(logger.set_error_directory(recorded.string()));
  logger.record(Level::Warning, "audio", "underrun");
  assert(read_lines(recorded / "audio.log").back().find("repeated") == std::string::npos);

  // Each category file stops at its byte limit, and says so once.
  const auto limited = root / "errors" / "2026-10-30_21-10-00";
  assert(logger.set_error_directory(limited.string(), Level::Warning, 400u));
  for (int i = 0; i < 50; ++i) {
    logger.record(Level::Error, "filesystem", "NtCreateFile failed: 'game:\\file" +
                                                  std::to_string(i) + ".bin'");
  }
  const auto filesystem_limited = read_lines(limited / "filesystem.log");
  assert(filesystem_limited.size() >= 2u && filesystem_limited.size() < 10u);
  assert(filesystem_limited.back().find("reached its size limit") != std::string::npos);
  assert(std::filesystem::file_size(limited / "filesystem.log") <= 400u + 128u);
  logger.record(Level::Error, "gpu", "another category is unaffected");
  assert(read_lines(limited / "gpu.log").size() == 1u);

  logger.set_error_directory("");
  logger.set_sink({});
  std::filesystem::remove_all(root);
  std::cout << "logger error file tests passed" << std::endl;
  return 0;
}
