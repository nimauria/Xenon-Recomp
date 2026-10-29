#pragma once
#include <nlohmann/json.hpp>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

namespace xenon::launcher_next {
// Filesystem work never runs under the launcher's command/state mutex.
class CaptureIndex {
public:
  CaptureIndex();
  ~CaptureIndex();
  void folder(const std::filesystem::path &root);
  void refresh();
  nlohmann::json snapshot() const;

private:
  void run() noexcept;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::thread worker_;
  std::filesystem::path root_;
  bool stopping_ = false, requested_ = false;
  std::uint64_t generation_ = 0;
  nlohmann::json result_ = {
      {"state", "idle"}, {"entries", nlohmann::json::array()}, {"truncated", false}};
};
bool capture_extension(const std::filesystem::path &file);
} // namespace xenon::launcher_next
