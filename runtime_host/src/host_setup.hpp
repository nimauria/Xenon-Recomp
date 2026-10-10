#pragma once

// Process setup for the runtime host: logging, the launch-configuration
// argument, and turning a LaunchConfig into a SessionConfig.

#include <cstddef>
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
bool read_host_file_bytes(const std::filesystem::path& path, std::vector<std::byte>& out_bytes);
xenon::core::SessionConfig build_session_config(const LaunchConfig& launch);

}  // namespace xenon::runtime_host
