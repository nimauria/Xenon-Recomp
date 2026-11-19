#pragma once
#include "platform/discord_rpc.hpp"
#include <condition_variable>
#include <mutex>
#include <thread>
namespace xenon::launcher_next {
inline constexpr auto presence_consent = "discord-ipc-v1";
Json presence_activity(const std::string &page, const Json &session, bool show_title);
class PresenceService {
public:
  PresenceService(std::string application_id, std::unique_ptr<IPresenceTransport> transport);
  ~PresenceService();
  bool available() const;
  void configure(bool requested, bool consented, bool safe, bool show_title,
                 const std::string &page, const Json &session);
  void refresh();
  Json snapshot() const;

private:
  void worker() noexcept;
  void status(const std::string &text, bool connected, bool published,
              const std::string &error = "");
  std::string application_id_;
  bool available_;
  std::unique_ptr<IPresenceTransport> transport_;
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  Json state_;
  std::uint64_t generation_ = 0;
  bool stopping_ = false, force_ = false;
  std::thread worker_;
};
Json community_information();
} // namespace xenon::launcher_next
