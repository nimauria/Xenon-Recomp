#include "launcher_service.hpp"
#include "platform/file_path.hpp"
namespace xenon::launcher_next {
namespace fs = std::filesystem;
std::unique_ptr<IRuntimeAdapter> LauncherService::configured_runtime(const Json &state) const {
  if (fixture_runtime_mode_)
    return {};
  auto executable = runtime_override_;
  if (executable.empty() && !state.value("runtimeHostPath", "").empty())
    executable = fs::u8path(state.at("runtimeHostPath").get<std::string>());
  if (executable.empty() && !runtime_installation_.empty())
#ifdef _WIN32
    executable = runtime_installation_ / "xenon_runtime_host.exe";
#else
    executable = runtime_installation_ / "xenon_runtime_host";
#endif
  if (executable.empty())
    return {};
  if (!executable.is_absolute())
    throw std::runtime_error("Choose an absolute runtime host path");
  return runtime_host(executable.lexically_normal(), storage_);
}
void LauncherService::configure_runtime(const fs::path &installation,
                                        const fs::path &explicit_host) {
  std::lock_guard lock(mutex_);
  if (fixture_runtime_mode_ && !explicit_host.empty())
    throw std::runtime_error("Choose either the fixture host or the real runtime host");
  runtime_installation_ = installation.empty() ? fs::path{} : fs::absolute(installation);
  runtime_override_ = explicit_host;
  if (!fixture_runtime_mode_)
    runtime_ = configured_runtime(state_);
}
} // namespace xenon::launcher_next
