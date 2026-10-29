#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
namespace xenon::launcher_next {
struct ProcessIdentity {
  std::uint64_t pid = 0;
  std::uint64_t created = 0;
};
ProcessIdentity launch_process(const std::filesystem::path &executable,
                               const std::vector<std::string> &arguments);
bool process_running(ProcessIdentity identity);
ProcessIdentity current_process_identity();
bool stop_process(ProcessIdentity identity);
void open_desktop_target(const std::string &target);
bool launch_detached(const std::filesystem::path &executable,
                     const std::vector<std::string> &arguments);
} // namespace xenon::launcher_next
