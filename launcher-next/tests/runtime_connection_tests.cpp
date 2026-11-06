#include "application/launcher_service.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>
using namespace xenon::launcher_next;
namespace fs = std::filesystem;
void check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
Json action(LauncherService &service, const std::string &name, Json data = Json::object()) {
  return service.request({{"id", make_id()},
                          {"version", 1},
                          {"command", "launcher.action"},
                          {"payload", {{"action", name}, {"data", data}}}});
}
Json wait_for(LauncherService &service, const std::string &state) {
  for (int i = 0; i < 500; ++i) {
    auto snapshot = service.snapshot();
    if (snapshot.value("session", Json::object()).value("state", "") == state)
      return snapshot;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("Runtime connection transition timed out: " + state);
}
int main(int argc, char **argv) {
  const auto root = fs::temp_directory_path() / ("xenon-runtime-connection-" + make_id());
  try {
    check(argc == 2, "Supply the explicit protocol test peer");
    const auto peer = fs::absolute(fs::u8path(argv[1]));
    fs::create_directories(root);
    const auto publication = root / "published.json";
    const std::string payload(128 * 1024, 'p');
    write_document(publication, {{"generation", 0}, {"payload", payload}});
    std::atomic_bool publishing{true}, publication_failed{false};
    std::string writer_error, reader_error;
    std::thread publisher([&] {
      try {
        for (int i = 1; i <= 32; ++i)
          write_document(publication, {{"generation", i}, {"payload", payload}});
      } catch (const std::exception &error) {
        writer_error = error.what();
        publication_failed = true;
      }
      publishing = false;
    });
    bool read_failed = false;
    do {
      try {
        check(read_document(publication).at("payload") == payload,
              "Read a complete atomic snapshot");
      } catch (const std::exception &error) {
        reader_error = error.what();
        read_failed = true;
      }
    } while (publishing);
    publisher.join();
    if (publication_failed || read_failed)
      throw std::runtime_error("Atomic publication: writer=" + writer_error +
                               " reader=" + reader_error);
    check(!publication_failed && !read_failed,
          "Readers must allow atomic publication to replace the file");
    {
      const auto log = root / "live.log";
      std::ofstream writer(log, std::ios::binary);
      writer << "first\n";
      writer.flush();
      check(read_text_tail(log, 65536) == "first\n",
            "Live runtime logs must be readable while the host owns the writer");
      writer << "second\n";
      writer.flush();
      check(read_text_tail(log, 7) == "second\n", "Runtime log tails must stay bounded");
    }
    const auto source = root / "contract-test.xex";
    std::ofstream(source) << "Protocol-only test content";
    std::string game;
    {
      LauncherService service(root / "data", false, {}, {}, nullptr);
      check(!service.snapshot()["capabilities"]["runtime"], "No runtime must be fabricated");
      service.configure_runtime(root);
      check(!service.snapshot()["runtimeConnection"]["available"],
            "An absent beside-launcher host stays unavailable");
      check(action(service, "runtime.configure", {{"path", path_string(peer)}}).at("ok"),
            "Save an explicit host");
      auto snapshot = service.snapshot();
      check(snapshot["runtimeConnection"]["source"] == "saved" &&
                snapshot["capabilities"]["runtime"] && !snapshot["session"]["fixture"],
            "Host selection must be saved and classified separately from fixture mode");
      check(!snapshot["runtimeServices"]["capabilities"]["input"],
            "Session integration must not fabricate input management");
      check(
          !action(service, "runtime.configure", {{"path", path_string(root / "missing")}}).at("ok"),
          "Reject missing hosts");
      check(service.snapshot()["runtimeConnection"]["configuredPath"] == path_string(peer),
            "Failed selection must be atomic");
      check(action(service, "library.import",
                   {{"paths", Json::array({path_string(source)})}, {"mode", "keep"}})
                .at("ok"),
            "Register test content");
      game = service.snapshot()["games"][0]["id"];
      check(action(service, "session.launch", {{"gameId", game}}).at("ok"),
            "Launch through the production transport");
      snapshot = wait_for(service, "running");
      check(snapshot["session"]["fixture"],
            "A peer that reports fixture mode must never count as a real game");
      const auto session = fs::u8path(snapshot["session"]["sessionDir"].get<std::string>());
      const auto config = read_document(session / "launch-config.json");
      check(config["configVersion"] == 1 && config["headlessMode"] == false &&
                config["fixture"] == false && config["inputModuleApiVersion"] == 1 &&
                config["inputUserSources"].is_array() && config["profileXuid"].is_string(),
            "The real host contract must use normal Play and exact field names/types");
      check(!action(service, "runtime.configure", {{"path", ""}}).at("ok"),
            "Live provider changes must fail");
      check(!action(service, "session.launch", {{"gameId", game}}).at("ok"),
            "Duplicate launches must fail");
      bool refused = false;
      try {
        auto incompatible = fixture_runtime(peer, root / "data");
      } catch (...) {
        refused = true;
      }
      check(refused, "Explicit fixture mode must not steal a live production-transport session");
    }
    {
      LauncherService reopened(root / "data", false, {}, {}, nullptr);
      auto snapshot = wait_for(reopened, "running");
      check(snapshot["session"]["gameId"] == game && snapshot["session"]["fixture"],
            "Reattach with the saved host and preserve fixture provenance");
      check(action(reopened, "session.stop").at("ok"), "Stop is delivered through stop.signal");
      wait_for(reopened, "stopped");
      check(reopened.snapshot()["games"][0]["playCount"] == 0,
            "Protocol peers cannot pollute real play counts");
      check(action(reopened, "runtime.configure", {{"path", ""}}).at("ok"),
            "Reset host selection after stopping");
    }
    {
      LauncherService options(root / "options", false, {}, {}, nullptr);
      options.configure_runtime({}, peer);
      check(options.snapshot()["runtimeConnection"]["locked"],
            "Command-line host selection takes precedence");
      check(!action(options, "runtime.configure", {{"path", ""}}).at("ok"),
            "Settings cannot silently override startup options");
      LauncherService fixture(root / "fixture", false, peer, {}, nullptr);
      check(!action(fixture, "runtime.configure", {{"path", path_string(peer)}}).at("ok"),
            "Demo fixtures cannot enable a real host");
      LauncherService safe(root / "safe", true, {}, {}, nullptr);
      safe.configure_runtime({}, peer);
      check(!safe.snapshot()["capabilities"]["runtime"],
            "Safe mode cannot enable runtime launching");
    }
    fs::remove_all(root);
    std::cout << "Production transport selection, atomic validation, contract mapping, "
                 "reattach/stop and fixture isolation passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
