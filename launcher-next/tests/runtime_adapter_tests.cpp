#include "adapters/runtime_adapter.hpp"
#include <chrono>
#include <iostream>
#include <thread>
using namespace xenon::launcher_next;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
Json wait_for(IRuntimeAdapter &adapter, const std::string &state) {
  for (int tick = 0; tick < 300; ++tick) {
    auto value = adapter.status();
    if (value.at("state") == state)
      return value;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("Runtime transition timed out: " + state);
}
int main(int argc, char **argv) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("xenon-runtime-adapter-" + make_id());
  try {
    check(argc == 2, "Fixture executable argument missing");
    const auto executable = std::filesystem::u8path(argv[1]);
    auto adapter = fixture_runtime(executable, root);
    check(adapter->launch(
                     {{"gameId", "scenario-normal"}, {"title", "Test session"}})
                  .at("state") == "starting",
          "Spawn must not imply Running");
    wait_for(*adapter, "running");
    bool rejected = false;
    try {
      adapter->launch({{"gameId", "duplicate"}});
    } catch (...) {
      rejected = true;
    }
    check(rejected, "Duplicate launch must fail");
    adapter.reset();
    adapter = fixture_runtime(executable, root);
    check(adapter->status().at("state") == "running",
          "Launcher must reattach to surviving host");
    check(adapter->stop().at("state") == "stopping",
          "Stop must be asynchronous");
    wait_for(*adapter, "stopped");
    adapter->launch({{"gameId", "scenario-bad-module"}});
    check(wait_for(*adapter, "failed").at("lastError") ==
              "Fixture module validation failure",
          "Host failures must propagate");
    adapter->launch({{"gameId", "scenario-crash"}});
    wait_for(*adapter, "running");
    check(wait_for(*adapter, "failed").at("lastError") ==
              "Runtime process exited without a terminal status",
          "Crash must not remain Running");
    adapter->launch({{"gameId", "scenario-slow-stop"}});
    wait_for(*adapter, "running");
    adapter->stop();
    wait_for(*adapter, "stopped");
    adapter.reset();
    std::filesystem::remove_all(root);
    std::cout << "Session lifecycle, duplicate launch, reattach, failure, "
                 "crash and slow stop passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
