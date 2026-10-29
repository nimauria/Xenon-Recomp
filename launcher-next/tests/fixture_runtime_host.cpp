// Launcher-only contract double. Does not load Xenon, modules or game code.
#include "core/state_file.hpp"
#include <chrono>
#include <fstream>
#include <thread>
using namespace xenon::launcher_next;
namespace fs = std::filesystem;
int main(int argc, char **argv) {
  try {
    if (argc != 3 || std::string(argv[1]) != "--launch-config")
      return 2;
    const auto config = read_document(fs::u8path(argv[2]));
    const auto directory = fs::u8path(config.at("sessionDir").get<std::string>());
    const auto id = config.at("sessionId");
    const auto scenario = config.value("gameId", "");
    std::int64_t running_at = 0;
    auto state = [&](const std::string &value, const std::string &error = "") {
      if (value == "running" && running_at == 0)
        running_at = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
      write_document(directory / "status.json", {{"sessionId", id},
                                                 {"stateName", value},
                                                 {"lastError", error},
                                                 {"fixture", true},
                                                 {"runningAtEpochMs", running_at}});
      std::ofstream(directory / "log.txt", std::ios::app) << value << '\n';
    };
    state("initializing");
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    state("ready");
    if (scenario == "scenario-bad-module") {
      state("failed", "Fixture module validation failure");
      return 1;
    }
    state("loading");
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    state("running");
    if (scenario == "scenario-crash") {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      std::_Exit(73);
    }
    for (int tick = 0; tick < 1000; ++tick) {
      if (fs::exists(directory / "stop.signal")) {
        state("stopping");
        if (scenario == "scenario-slow-stop")
          std::this_thread::sleep_for(std::chrono::milliseconds(600));
        state("stopped");
        return 0;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    state("stopped");
    return 0;
  } catch (...) {
    return 2;
  }
}
