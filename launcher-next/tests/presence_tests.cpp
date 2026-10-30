#include "application/launcher_service.hpp"
#include "core/presence.hpp"
#include "core/state_file.hpp"
#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
using namespace xenon::launcher_next;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace {
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class F> void eventually(F condition, const char *message) {
  const auto deadline = std::chrono::steady_clock::now() + 4s;
  while (!condition()) {
    if (std::chrono::steady_clock::now() > deadline)
      throw std::runtime_error(message);
    std::this_thread::sleep_for(10ms);
  }
}
struct Recording {
  std::mutex mutex;
  int opens = 0, closes = 0;
  bool found = true, acknowledge = true, reject = false;
  std::vector<RpcFrame> sent, incoming;
};
class Fake final : public IPresenceTransport {
public:
  explicit Fake(std::shared_ptr<Recording> record) : record_(std::move(record)) {}
  bool open() override {
    std::lock_guard lock(record_->mutex);
    ++record_->opens;
    return record_->found;
  }
  void close() override {
    std::lock_guard lock(record_->mutex);
    ++record_->closes;
  }
  void send(const RpcFrame &frame) override {
    std::lock_guard lock(record_->mutex);
    record_->sent.push_back(frame);
    if (frame.opcode == 0)
      record_->incoming.push_back({1, {{"cmd", "DISPATCH"}, {"evt", "READY"}}});
    if (frame.opcode == 1 && frame.payload.value("cmd", "") == "SET_ACTIVITY" &&
        record_->acknowledge)
      record_->incoming.push_back({1,
                                   {{"cmd", "SET_ACTIVITY"},
                                    {"nonce", frame.payload.at("nonce")},
                                    {"evt", record_->reject ? Json("ERROR") : Json()}}});
  }
  std::vector<RpcFrame> poll() override {
    std::lock_guard lock(record_->mutex);
    auto result = std::move(record_->incoming);
    record_->incoming.clear();
    return result;
  }

private:
  std::shared_ptr<Recording> record_;
};
Json action(LauncherService &service, const std::string &name, Json data = Json::object()) {
  return service.request({{"version", 1},
                          {"id", "test"},
                          {"command", "launcher.action"},
                          {"payload", {{"action", name}, {"data", data}}}});
}
} // namespace
int main() {
  const auto root = fs::temp_directory_path() / ("xenon-presence-" + make_id());
  try {
    const auto encoded = encode_rpc_frame({1, {{"cmd", "SET_ACTIVITY"}, {"nonce", "abc"}}});
    std::vector<unsigned char> buffer(encoded.begin(), encoded.begin() + 9);
    check(decode_rpc_frames(buffer).empty(), "Partial frames must be retained");
    buffer.insert(buffer.end(), encoded.begin() + 9, encoded.end());
    auto frames = decode_rpc_frames(buffer);
    check(frames.size() == 1 && frames[0].payload.at("nonce") == "abc" && buffer.empty(),
          "Fragmented frame decode");
    const std::string ping("\0raw\xff", 5);
    buffer = encode_rpc_frame({3, nullptr, ping});
    frames = decode_rpc_frames(buffer);
    check(frames[0].raw == ping, "Ping payload must remain byte-exact");
    buffer = {1, 0, 0, 0, 1, 0, 1, 0};
    bool rejected = false;
    try {
      decode_rpc_frames(buffer);
    } catch (...) {
      rejected = true;
    }
    check(rejected, "Oversized IPC frames must be rejected before allocation");
    Json session = {{"state", "running"},
                    {"title", "Private title"},
                    {"contentPath", "SECRET_PATH"},
                    {"profileName", "SECRET_PROFILE"},
                    {"startedAtEpochMs", 1234000}};
    const auto hidden = presence_activity("Home", session, false);
    check(hidden.at("details") == "Playing a game through Xenon" &&
              hidden.at("timestamps").at("start") == 1234,
          "Private title and millisecond conversion");
    check(hidden.dump().find("SECRET") == std::string::npos, "No private paths or profile fields");
    auto record = std::make_shared<Recording>();
    {
      PresenceService presence("1550625950570520768", std::make_unique<Fake>(record));
      presence.configure(true, false, false, true, "Library", session);
      std::this_thread::sleep_for(150ms);
      {
        std::lock_guard lock(record->mutex);
        check(record->opens == 0, "No IPC connection before explicit consent");
      }
      {
        std::lock_guard lock(record->mutex);
        record->acknowledge = false;
      }
      presence.configure(true, true, false, false, "Home", session);
      eventually([&] { return presence.snapshot().at("connected") == true; }, "Handshake ready");
      eventually(
          [&] {
            std::lock_guard lock(record->mutex);
            return !record->sent.empty() && record->sent.back().opcode == 1;
          },
          "Initial activity queued");
      check(presence.snapshot().at("published") == false, "Connected does not mean published");
      {
        std::lock_guard lock(record->mutex);
        check(record->sent.front().opcode == 0 && record->sent.front().payload.at("v") == 1,
              "Versioned handshake");
        const auto &sent = record->sent.back();
        check(sent.payload.at("args").at("activity") == hidden, "Exact privacy-filtered activity");
        record->incoming.push_back(
            {1, {{"cmd", "SET_ACTIVITY"}, {"nonce", sent.payload.at("nonce")}, {"evt", nullptr}}});
        record->incoming.push_back({3, nullptr, ping});
        record->acknowledge = true;
      }
      eventually([&] { return presence.snapshot().at("published") == true; },
                 "Acknowledged publication");
      eventually(
          [&] {
            std::lock_guard lock(record->mutex);
            return record->sent.back().opcode == 4 && record->sent.back().raw == ping;
          },
          "Byte-exact pong");
      presence.configure(true, true, true, false, "Home", session);
      eventually([&] { return presence.snapshot().at("connected") == false; },
                 "Safe mode disconnects");
      {
        std::lock_guard lock(record->mutex);
        check(record->sent.back().payload.at("args").at("activity").is_null(),
              "Clear activity on suppression");
      }
      check(!presence.snapshot().at("enabled").get<bool>() &&
                presence.snapshot().at("requestedEnabled") == true,
            "Safe mode preserves explicit opt-in but suppresses activity");
      {
        std::lock_guard lock(record->mutex);
        record->reject = true;
      }
      presence.configure(true, true, false, true, "Profiles", Json::object());
      presence.refresh();
      eventually([&] { return !presence.snapshot().value("lastPublishError", "").empty(); },
                 "Provider rejection surfaced");
      check(!presence.snapshot().at("published").get<bool>(), "Rejected activity is not published");
      {
        std::lock_guard lock(record->mutex);
        record->reject = false;
      }
      presence.refresh();
      eventually([&] { return presence.snapshot().at("published") == true; },
                 "Explicit reconnect recovers");
    }
    {
      std::lock_guard lock(record->mutex);
      check(record->sent.back().payload.at("args").at("activity").is_null(),
            "Shutdown clears activity");
    }
    auto service_record = std::make_shared<Recording>();
    {
      LauncherService service(root, false, {}, {}, platform_http(),
                              std::make_unique<Fake>(service_record));
      check(service.snapshot().at("community").at("presence").at("enabled") == false,
            "Sharing defaults off");
      check(action(service, "settings.set",
                   {{"key", "community/discordRichPresence"}, {"value", true}})
                    .at("ok") == true,
            "Explicit opt-in accepted");
      check(!service.snapshot().at("settings").contains("community/presenceConsent"),
            "Consent marker remains native-owned");
      check(action(service, "settings.set",
                   {{"key", "community/presenceConsent"}, {"value", presence_consent}})
                    .at("ok") == false,
            "Renderer cannot write consent marker");
      action(service, "application.ready");
    }
    {
      LauncherService service(root, true, {}, {}, platform_http(),
                              std::make_unique<Fake>(service_record));
      const auto presence = service.snapshot().at("community").at("presence");
      check(presence.at("requestedEnabled") == true && presence.at("enabled") == false,
            "Persisted consent suppressed in safe mode");
      check(action(service, "settings.set",
                   {{"key", "community/discordRichPresence"}, {"value", true}})
                    .at("ok") == false,
            "Safe-mode enabling blocked");
      action(service, "application.ready");
    }
    {
      LauncherService service(root, false, {}, {}, platform_http(),
                              std::make_unique<Fake>(service_record));
      check(service.snapshot().at("community").at("presence").at("enabled") == true,
            "Valid explicit opt-in survives restart");
      check(action(service, "settings.set",
                   {{"key", "community/discordRichPresence"}, {"value", false}})
                    .at("ok") == true,
            "Opt-out accepted");
    }
    auto state = read_document(root / "state.json");
    check(!state.at("settings").contains("community/presenceConsent"),
          "Opt-out removes stored consent");
    state["settings"]["community/discordRichPresence"] = true;
    write_document(root / "state.json", state);
    {
      LauncherService service(root, false, {}, {}, platform_http(),
                              std::make_unique<Fake>(service_record));
      check(service.snapshot().at("settings").at("community/discordRichPresence") == false,
            "Legacy opt-in requires fresh consent");
    }
    fs::remove_all(root);
    std::cout << "presence: ok\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
