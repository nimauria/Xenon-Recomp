#pragma once
#include "adapters/runtime_adapter.hpp"
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>

namespace xenon::launcher_next {
using Json = nlohmann::json;
class LauncherService {
public:
  explicit LauncherService(std::filesystem::path storage, bool safe_mode = false,
                           std::filesystem::path fixture_host = {});
  ~LauncherService();
  Json request(const Json &request);
  Json snapshot();
  Json presentation_settings() const;
  std::optional<std::filesystem::path> asset_path(const std::string &category,
                                                  const std::string &id) const;
  const std::filesystem::path &storage() const { return storage_; }

private:
  Json execute(const std::string &command, const Json &payload);
  void persist(const Json &next);
  void load();
  void validate(const Json &state) const;
  void notify(Json &next, const std::string &title, const std::string &message);
  void poll_runtime();
  Json project() const;
  Json launch_configuration(const Json &state, const std::string &game_id) const;
  std::filesystem::path storage_;
  Json state_;
  bool safe_mode_ = false;
  bool recovery_needed_ = false;
  std::unique_ptr<IRuntimeAdapter> runtime_;
  std::string dismissed_session_;
  mutable std::mutex mutex_;
};
std::filesystem::path default_storage();
std::string path_string(const std::filesystem::path &path);
} // namespace xenon::launcher_next
