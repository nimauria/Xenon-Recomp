#pragma once

// Process setup for the runtime host: logging, the launch-configuration
// argument, and turning a LaunchConfig into a SessionConfig.

#include <cstddef>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "launch_config.hpp"
#include "xenon/core/session.hpp"

namespace xenon::runtime_host {

void log_host_capabilities();
std::optional<std::string> parse_launch_config_argument(int argc, char** argv);
void redirect_log(const std::string& session_dir);

// Per-run error logs: <session_dir>/errors/<local start time>/<category>.log
// (gpu.log, filesystem.log, ...), one folder per run named
// "YYYY-MM-DD_HH-MM-SS" (with "-2", "-3"... if that name is taken), so runs
// sort by name. Keeps only the newest `keep_runs` run folders. Points the
// logger's per-category error files at the new folder and returns it, or an
// empty path if it could not be created.
inline constexpr std::size_t kErrorLogRunsKept = 20;
std::filesystem::path open_error_logs(const std::string& session_dir,
                                      std::size_t keep_runs = kErrorLogRunsKept);
// The run folder name for a local time, e.g. "2026-10-30_20-15-02".
std::string error_log_run_name(const std::tm& local_time);
// Removes the oldest run folders under `errors_dir` until at most `keep_runs`
// remain; anything that is not a run folder is left alone.
void prune_error_log_runs(const std::filesystem::path& errors_dir, std::size_t keep_runs);
bool read_host_file_bytes(const std::filesystem::path& path, std::vector<std::byte>& out_bytes);
xenon::core::SessionConfig build_session_config(const LaunchConfig& launch);

}  // namespace xenon::runtime_host
