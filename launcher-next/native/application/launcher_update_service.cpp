#include "launcher_service.hpp"
#include "core/launcher_update.hpp"
#include "core/releases.hpp"
namespace xenon::launcher_next {
namespace fs = std::filesystem;
void LauncherService::configure_update_installation(const fs::path &installation) {
  std::lock_guard lock(mutex_);
  if (!safe_mode_ && update_installation_supported(installation))
    update_installation_ = installation;
}
void LauncherService::update_handoff_failed(const std::string &message) {
  std::lock_guard lock(mutex_);
  update_preparing_ = false;
  auto next = state_;
  next["launcherUpdate"]["state"] = "error";
  next["launcherUpdate"]["lastError"] = message;
  next["revision"] = state_["revision"].get<std::int64_t>() + 1;
  persist(next);
  state_ = std::move(next);
}
fs::path LauncherService::prepare_update_installation(ProcessIdentity parent) {
  Json update;
  fs::path installation;
  {
    std::lock_guard lock(mutex_);
    if (safe_mode_ || update_installation_.empty())
      throw std::runtime_error("Automatic replacement requires a packaged Windows installation");
    if (update_preparing_)
      throw std::runtime_error("Launcher replacement is already being prepared");
    if (runtime_) {
      const auto status = runtime_->status().value("state", "idle");
      if (status != "idle" && status != "unavailable" && status != "stopped" && status != "failed")
        throw std::runtime_error("Stop the game before updating the launcher");
    }
    for (const auto &job : state_["jobs"]) {
      const auto status = job.value("state", "");
      if (status == "queued" || status == "running" || status == "cancelling" ||
          status == "committing" || job.value("_cleanup", false))
        throw std::runtime_error("Wait for file operations to finish before updating the launcher");
    }
    update = state_.at("launcherUpdate");
    if (!newer_version(update.value("downloadVersion", ""), launcher_version))
      throw std::runtime_error(
          "The staged launcher release must be newer than the running application");
    installation = update_installation_;
    const auto id = update.at("_downloadId").get<std::string>();
    if (id.empty() || id.size() > 100 ||
        id.find_first_not_of("0123456789abcdef-") != std::string::npos ||
        fs::u8path(update.at("_packagePath").get<std::string>()) !=
            storage_ / "downloads" / id / "package.zip")
      throw std::runtime_error("The staged launcher package no longer matches its download record");
    if (update.at("downloadVersion") != update.at("release").at("version") ||
        update.at("downloadDigest") != update.at("release").at("asset").at("sha256"))
      throw std::runtime_error(
          "Download the currently selected launcher release before installing");
    auto next = state_;
    next["launcherUpdate"]["state"] = "preparing";
    next["revision"] = state_["revision"].get<std::int64_t>() + 1;
    persist(next);
    state_ = std::move(next);
    update_preparing_ = true;
  }
  try {
    const auto plan = prepare_launcher_update(
        fs::u8path(update.at("_packagePath").get<std::string>()), update.at("downloadDigest"),
        update.at("downloadVersion"), installation, storage_, parent);
    {
      std::lock_guard lock(mutex_);
      auto next = state_;
      next["launcherUpdate"]["state"] = "restarting";
      next["launcherUpdate"]["_installPlan"] = path_string(plan);
      next["revision"] = state_["revision"].get<std::int64_t>() + 1;
      persist(next);
      state_ = std::move(next);
    }
    return plan;
  } catch (const std::exception &error) {
    update_handoff_failed(error.what());
    throw;
  }
}
} // namespace xenon::launcher_next
