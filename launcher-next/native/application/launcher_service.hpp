#pragma once
#include "adapters/runtime_adapter.hpp"
#include "adapters/host_services.hpp"
#include "platform/http.hpp"
#include "capture_index.hpp"
#include "platform/storage_lock.hpp"
#include "platform/desktop.hpp"
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <atomic>
#include <condition_variable>
#include <thread>
#include <functional>

namespace xenon::launcher_next {
using Json = nlohmann::json;
class LauncherService {
public:
  explicit LauncherService(
      std::filesystem::path storage, bool safe_mode = false,
      std::filesystem::path fixture_host = {},
      std::function<void(const std::string &, const std::string &)> install_observer = {},
      std::shared_ptr<IHttpClient> http = platform_http());
  ~LauncherService();
  Json request(const Json &request);
  Json snapshot();
  // Observer must only enqueue work; called while the service state is locked.
  void observe_changes(std::function<void(std::int64_t)> observer);
  Json presentation_settings() const;
  void configure_update_installation(const std::filesystem::path &installation);
  std::filesystem::path prepare_update_installation(ProcessIdentity parent);
  void update_handoff_failed(const std::string &message);
  std::optional<std::filesystem::path> asset_path(const std::string &category,
                                                  const std::string &id) const;
  const std::filesystem::path &storage() const { return storage_; }

private:
  void cache_catalog_art(Json &entry, const std::shared_ptr<std::atomic_bool> &cancel);
  Json execute(const std::string &command, const Json &payload);
  void persist(const Json &next);
  void load();
  void validate(const Json &state) const;
  void notify(Json &next, const std::string &title, const std::string &message,
              const std::string &severity = "info", Json action = Json::object());
  void poll_runtime();
  Json project() const;
  Json launch_configuration(const Json &state, const std::string &game_id) const;
  void enqueue_install(Json &next, const std::filesystem::path &source);
  void recover_install_jobs();
  void jobs_worker() noexcept;
  void run_install(const Json &job, const std::shared_ptr<std::atomic_bool> &cancel);
  void update_job(const std::string &id, const Json &fields);
  void cleanup_install(const Json &job);
  void register_installed_module(Json &next, const Json &definition,
                                 const std::filesystem::path &module_path);
  void load_online_state();
  void project_online(Json &result) const;
  void enqueue_network(Json &next, const std::string &kind, const std::string &module_id = "");
  void run_network(const Json &job, const std::shared_ptr<std::atomic_bool> &cancel);
  void fail_network(const Json &job, const std::string &message);
  void apply_online_error(Json &next, const Json &job, const std::string &message);
  void cleanup_download(const Json &job);
  void complete_job(const std::string &id, const std::function<void(Json &)> &mutation,
                    bool finish = true);
  void enqueue_library(Json &next, const std::string &kind, const Json &data);
  void run_library(const Json &job, const std::shared_ptr<std::atomic_bool> &cancel);
  void cleanup_library(const Json &job);
  void require_library_idle(const Json &state, const std::string &game_id) const;
  void execute_media(Json &next, const std::string &command, const Json &data);
  void enqueue_migration(Json &next, const std::string &kind, const Json &data);
  void run_migration(const Json &job, const std::shared_ptr<std::atomic_bool> &cancel);
  void recover_migrations();
  void enqueue_service(Json &next, const std::string &command, Json data);
  void run_service(const Json &job, const std::shared_ptr<std::atomic_bool> &cancel);
  void run_module_scan(const Json &job, const std::shared_ptr<std::atomic_bool> &cancel);
  void refresh_module_presentations(Json &next);
  Json fetch_json(const std::string &url, const std::shared_ptr<std::atomic_bool> &cancel);
  std::filesystem::path storage_;
  StorageLock storage_lock_;
  Json state_;
  bool safe_mode_ = false;
  bool recovery_needed_ = false;
  std::unique_ptr<IRuntimeAdapter> runtime_;
  std::unique_ptr<IHostServices> host_services_;
  std::string dismissed_session_;
  mutable std::mutex mutex_;
  std::condition_variable jobs_changed_;
  std::thread job_worker_;
  bool jobs_stopping_ = false;
  std::string running_job_;
  std::shared_ptr<std::atomic_bool> cancel_job_;
  std::function<void(const std::string &, const std::string &)> install_observer_;
  std::shared_ptr<IHttpClient> http_;
  bool online_initialized_ = false;
  std::function<void(std::int64_t)> state_observer_;
  mutable CaptureIndex captures_;
  std::filesystem::path update_installation_;
  bool update_preparing_ = false;
};
std::filesystem::path default_storage();
std::string path_string(const std::filesystem::path &path);
bool network_job_kind(const std::string &kind);
bool migration_job_kind(const std::string &kind);
bool library_job_kind(const std::string &kind);
bool supported_job_kind(const std::string &kind);
} // namespace xenon::launcher_next
