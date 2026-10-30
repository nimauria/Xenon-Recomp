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

  logger.set_error_directory("");
  logger.set_sink({});
  std::filesystem::remove_all(root);
  std::cout << "logger error file tests passed" << std::endl;
  return 0;
}
