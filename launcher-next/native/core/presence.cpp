#include "presence.hpp"
#include "platform/desktop.hpp"
#include <chrono>
#include <regex>
namespace xenon::launcher_next {
namespace {
std::string bounded(std::string text) {
  if (text.size() <= 128)
    return text;
  std::size_t end = 128;
  while (end && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
    --end;
  return text.substr(0, end);
}
} // namespace
Json community_information() {
  return {{"name", "Project Xenon"},
          {"discordUrl", "https://discord.gg/zVGw3HADgA"},
          {"projectUrl", "https://github.com/nimauria/Xenon-Recomp"},
          {"presenceProvider", "discord-ipc"},
          {"consentVersion", presence_consent}};
}
Json presence_activity(const std::string &page, const Json &session, bool show_title) {
  const auto state = session.value("state", "idle");
  const bool active = state == "preparing" || state == "validating" || state == "starting" ||
                      state == "running" || state == "stopping";
  std::string details = page == "Home"       ? "Viewing Xenon activity"
                        : page == "Modules"  ? "Managing Xenon modules"
                        : page == "Profiles" ? "Managing launcher profiles"
                        : page == "Settings" ? "Customising Xenon"
                                             : "Browsing the game library";
  if (active) {
    const auto title = show_title ? session.value("title", "") : "";
    const auto subject = title.empty() ? "a game through Xenon" : title;
    details = (state == "running"    ? "Playing "
               : state == "stopping" ? "Stopping "
                                     : "Launching ") +
              subject;
  }
  Json result = {
      {"type", 0},
      {"details", bounded(details)},
      {"state", "Powered by Xenon"},
      {"buttons",
       Json::array(
           {{{"label", "Join Xenon Discord"}, {"url", "https://discord.gg/zVGw3HADgA"}},
            {{"label", "Project Xenon"}, {"url", "https://github.com/nimauria/Xenon-Recomp"}}})}};
  if (active && state == "running" && session.contains("startedAtEpochMs") &&
      session["startedAtEpochMs"].is_number())
    if (const auto timestamp = session["startedAtEpochMs"].get<std::int64_t>(); timestamp > 0)
      result["timestamps"] = {{"start", timestamp / 1000}};
  return result;
}
PresenceService::PresenceService(std::string application_id,
                                 std::unique_ptr<IPresenceTransport> transport)
    : application_id_(std::move(application_id)),
      available_(std::regex_match(application_id_, std::regex("[1-9][0-9]{5,19}"))),
      transport_(std::move(transport)) {
  available_ = available_ && transport_ != nullptr;
  state_ = {{"enabled", false},
            {"requestedEnabled", false},
            {"suppressed", false},
            {"providerId", "discord-ipc"},
            {"providerAvailable", available_},
            {"applicationIdConfigured", available_},
            {"showGameTitle", true},
            {"page", "Library"},
            {"activity", presence_activity("Library", Json::object(), true)},
            {"providerStatus", available_ ? "Disabled" : "Unavailable"},
            {"connected", false},
            {"published", false},
            {"lastPublishError", ""}};
  worker_ = std::thread([this] { worker(); });
}
PresenceService::~PresenceService() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  changed_.notify_all();
  if (worker_.joinable())
    worker_.join();
}
bool PresenceService::available() const { return available_; }
void PresenceService::configure(bool requested, bool consented, bool safe, bool show_title,
                                const std::string &page, const Json &session) {
  const auto activity = presence_activity(page, session, show_title);
  const bool enabled = available_ && requested && consented && !safe;
  std::lock_guard lock(mutex_);
  if (state_["enabled"] == enabled && state_["requestedEnabled"] == requested &&
      state_["suppressed"] == safe && state_["showGameTitle"] == show_title &&
      state_["page"] == page && state_["activity"] == activity)
    return;
  state_["enabled"] = enabled;
  state_["requestedEnabled"] = requested;
  state_["suppressed"] = safe;
  state_["showGameTitle"] = show_title;
  state_["page"] = page;
  state_["activity"] = activity;
  ++generation_;
  changed_.notify_all();
}
void PresenceService::refresh() {
  std::lock_guard lock(mutex_);
  force_ = true;
  ++generation_;
  changed_.notify_all();
}
Json PresenceService::snapshot() const {
  std::lock_guard lock(mutex_);
  return state_;
}
void PresenceService::status(const std::string &text, bool connected, bool published,
                             const std::string &error) {
  std::lock_guard lock(mutex_);
  state_["providerStatus"] = text;
  state_["connected"] = connected;
  state_["published"] = published;
  state_["lastPublishError"] = bounded(error);
}
void PresenceService::worker() noexcept {
  using namespace std::chrono;
  bool open = false, ready = false;
  std::uint64_t observed = 0, sent = ~std::uint64_t{0};
  std::string pending;
  steady_clock::time_point retry{}, handshake{}, next_publish{}, acknowledgement{};
  const auto send_activity = [&](Json activity, const std::string &nonce) {
    transport_->send(
        {1,
         {{"cmd", "SET_ACTIVITY"},
          {"nonce", nonce},
          {"args", {{"pid", current_process_identity().pid}, {"activity", std::move(activity)}}}}});
  };
  for (;;) {
    Json desired;
    bool stop, force;
    std::uint64_t generation;
    {
      std::unique_lock lock(mutex_);
      if (!state_.value("enabled", false) && !open)
        changed_.wait(lock, [&] { return stopping_ || generation_ != observed; });
      else
        changed_.wait_for(lock, milliseconds(100),
                          [&] { return stopping_ || generation_ != observed || force_; });
      desired = state_;
      stop = stopping_;
      force = force_;
      force_ = false;
      generation = generation_;
      observed = generation;
    }
    try {
      if (stop || !desired.value("enabled", false)) {
        if (open && ready)
          send_activity(nullptr, make_id());
        if (transport_)
          transport_->close();
        open = ready = false;
        pending.clear();
        sent = ~std::uint64_t{0};
        retry = {};
        status(available_
                   ? desired.value("suppressed", false) ? "Suppressed in safe mode" : "Disabled"
                   : "Unavailable",
               false, false);
        if (stop)
          return;
        continue;
      }
      const auto now = steady_clock::now();
      if (force) {
        retry = {};
        sent = ~std::uint64_t{0};
        next_publish = {};
      }
      if (!open) {
        if (now < retry)
          continue;
        if (!transport_->open()) {
          status("Discord desktop client not found", false, false);
          retry = now + seconds(5);
          continue;
        }
        open = true;
        ready = false;
        sent = ~std::uint64_t{0};
        pending.clear();
        next_publish = {};
        transport_->send({0, {{"v", 1}, {"client_id", application_id_}}});
        handshake = now + seconds(3);
        status("Connecting to Discord", false, false);
      }
      for (const auto &frame : transport_->poll()) {
        if (frame.opcode == 3) {
          transport_->send({4, frame.payload, frame.raw});
          continue;
        }
        if (frame.opcode == 2)
          throw std::runtime_error("Discord closed the presence connection");
        if (frame.opcode != 1 || !frame.payload.is_object())
          continue;
        const auto event = frame.payload.value("evt", Json());
        if (event == "READY" && frame.payload.value("cmd", "") == "DISPATCH") {
          ready = true;
          status("Connected to Discord", true, false);
        }
        if (!pending.empty() && frame.payload.value("nonce", Json()) == pending) {
          if (event == "ERROR")
            throw std::runtime_error("Discord rejected the presence update");
          if (frame.payload.value("cmd", "") == "SET_ACTIVITY") {
            pending.clear();
            status("Rich Presence published to Discord", true, true);
          }
        }
      }
      if (!ready && now > handshake)
        throw std::runtime_error("Discord presence handshake timed out");
      if (!pending.empty() && now > acknowledgement)
        throw std::runtime_error("Discord did not acknowledge the presence update");
      if (ready && pending.empty() && sent != generation && now >= next_publish) {
        pending = make_id();
        send_activity(desired.at("activity"), pending);
        sent = generation;
        next_publish = now + seconds(1);
        acknowledgement = now + seconds(5);
        status("Presence update queued", true, false);
      }
    } catch (const std::exception &error) {
      if (transport_)
        transport_->close();
      open = ready = false;
      pending.clear();
      status("Discord presence disconnected", false, false, error.what());
      retry = steady_clock::now() + seconds(5);
      if (stop)
        return;
    } catch (...) {
      if (transport_)
        transport_->close();
      open = ready = false;
      pending.clear();
      status("Discord presence disconnected", false, false, "Unexpected presence provider failure");
      retry = steady_clock::now() + seconds(5);
      if (stop)
        return;
    }
  }
}
} // namespace xenon::launcher_next
