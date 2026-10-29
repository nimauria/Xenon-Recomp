#pragma once
#include "core/state_file.hpp"
#include "platform/desktop.hpp"
#include <functional>
namespace xenon::launcher_next {
inline constexpr auto launcher_version = XENON_LAUNCHER_VERSION;
bool update_installation_supported(const std::filesystem::path &installation);
Json launcher_update_status(const std::filesystem::path &plan);
std::filesystem::path prepare_launcher_update(const std::filesystem::path &package,
                                              const std::string &digest, const std::string &version,
                                              const std::filesystem::path &installation,
                                              const std::filesystem::path &storage,
                                              ProcessIdentity parent);
struct UpdateHooks {
  std::function<ProcessIdentity(const std::filesystem::path &, const std::vector<std::string> &)>
      launch = launch_process;
  std::function<bool(ProcessIdentity)> running = process_running;
  std::function<bool(ProcessIdentity)> stop = stop_process;
  std::function<void(const std::string &)> phase;
  unsigned parent_wait_ms = 120000;
  unsigned ready_wait_ms = 60000;
};
// The helper runs outside the installation. Backups and failed payloads are retained.
void apply_launcher_update(const std::filesystem::path &plan, const UpdateHooks &hooks = {});
void acknowledge_launcher_update(const std::filesystem::path &receipt, const std::string &token);
} // namespace xenon::launcher_next
