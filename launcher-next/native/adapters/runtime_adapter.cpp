#include "runtime_adapter.hpp"
#include "platform/desktop.hpp"
#include <chrono>
#include <fstream>
#include <stdexcept>
namespace xenon::launcher_next {
namespace fs = std::filesystem;
namespace {
std::int64_t epoch() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
bool terminal(const std::string &state) { return state == "stopped" || state == "failed"; }
class FileRuntime final : public IRuntimeAdapter {
public:
  FileRuntime(fs::path executable, fs::path storage)
      : executable_(fs::absolute(std::move(executable))),
        storage_(fs::absolute(std::move(storage))) {
    if (fs::exists(storage_ / "active-session.json")) {
      const auto record = read_document(storage_ / "active-session.json");
      const auto directory = fs::u8path(record.at("sessionDir").get<std::string>());
      const auto root = fs::weakly_canonical(storage_ / "sessions");
      if (fs::weakly_canonical(directory).parent_path() != root)
        throw std::runtime_error("Saved session directory is outside launcher storage");
      session_ = record;
    }
  }
  bool available() const override { return fs::is_regular_file(executable_); }
  Json status() override {
    if (session_.empty())
      return {{"state", "idle"}, {"fixture", true}};
    auto state = session_.value("state", "starting");
    const auto directory = fs::u8path(session_.at("sessionDir").get<std::string>());
    try {
      if (fs::exists(directory / "status.json")) {
        const auto report = read_document(directory / "status.json", 1024 * 1024);
        if (report.at("sessionId") == session_.at("sessionId")) {
          const auto reported = report.at("stateName").get<std::string>();
          if (reported == "initializing" || reported == "ready" || reported == "loading" ||
              reported == "loading-game" || reported == "running" || reported == "paused" ||
              reported == "stopping" || terminal(reported)) {
            state = reported;
            session_["state"] = state;
            session_["lastError"] = report.value("lastError", "");
          }
        }
      }
    } catch (const std::exception &) { /* A transient read never invents a Running state. */
    }
    if (fs::exists(directory / "stop.signal") && !terminal(state)) {
      state = "stopping";
      session_["state"] = state;
    }
    const ProcessIdentity identity{session_.value("pid", std::uint64_t{}),
                                   session_.value("created", std::uint64_t{})};
    if (!terminal(state) && !process_running(identity)) {
      session_["state"] = "failed";
      session_["lastError"] = "Runtime process exited without a terminal status";
    } else if (state == "starting" &&
               epoch() - session_.at("startedAtEpochMs").get<std::int64_t>() > 30000) {
      session_["state"] = "failed";
      session_["lastError"] = "Runtime did not report its startup state in time";
    }
    if (terminal(session_.value("state", "")) && !session_.contains("endedAtEpochMs")) {
      session_["endedAtEpochMs"] = epoch();
      write_document(storage_ / "active-session.json", session_);
    }
    const auto log = directory / "log.txt";
    if (fs::is_regular_file(log)) {
      const auto size = fs::file_size(log);
      std::ifstream input(log, std::ios::binary);
      if (size > 65536)
        input.seekg(static_cast<std::streamoff>(size - 65536));
      session_["logTail"] = std::string(std::istreambuf_iterator<char>(input), {});
    }
    return session_;
  }
  Json launch(Json configuration) override {
    if (!available())
      throw std::runtime_error("Fixture runtime executable is missing");
    if (!session_.empty() && !terminal(status().value("state", "idle")))
      throw std::runtime_error("A runtime session is already active");
    const auto id = make_id();
    const auto directory = storage_ / "sessions" / id;
    fs::create_directories(directory);
    configuration["configVersion"] = 1;
    configuration["sessionId"] = id;
    const auto utf8 = directory.u8string();
    configuration["sessionDir"] = std::string(utf8.begin(), utf8.end());
    const auto file = directory / "launch-config.json";
    write_document(file, configuration);
    const auto path = file.u8string();
    const auto process =
        launch_process(executable_, {"--launch-config", std::string(path.begin(), path.end())});
    session_ = {{"sessionId", id},
                {"sessionDir", configuration.at("sessionDir")},
                {"gameId", configuration.value("gameId", "")},
                {"title", configuration.value("title", "")},
                {"state", "starting"},
                {"fixture", true},
                {"pid", process.pid},
                {"created", process.created},
                {"startedAtEpochMs", epoch()},
                {"lastError", ""}};
    try {
      write_document(storage_ / "active-session.json", session_);
    } catch (...) {
      std::ofstream(directory / "stop.signal") << "stop";
      throw;
    }
    return session_; // Process creation is not evidence of a Running game.
  }
  Json stop() override {
    if (session_.empty() || terminal(status().value("state", "idle")))
      throw std::runtime_error("No active runtime session");
    std::ofstream signal(fs::u8path(session_.at("sessionDir").get<std::string>()) / "stop.signal");
    signal << "stop";
    signal.close();
    if (!signal)
      throw std::runtime_error("Could not request a runtime stop");
    session_["state"] = "stopping";
    return session_;
  }

private:
  fs::path executable_, storage_;
  Json session_ = Json::object();
};
} // namespace
std::unique_ptr<IRuntimeAdapter> fixture_runtime(fs::path executable, fs::path storage) {
  return std::make_unique<FileRuntime>(std::move(executable), std::move(storage));
}
} // namespace xenon::launcher_next
