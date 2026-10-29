#include "application/capture_index.hpp"
#include "core/state_file.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
using namespace xenon::launcher_next;
namespace fs = std::filesystem;
void check(bool ok, const char *message) {
  if (!ok)
    throw std::runtime_error(message);
}
nlohmann::json ready(CaptureIndex &index) {
  for (unsigned attempt = 0; attempt < 1000; ++attempt) {
    const auto result = index.snapshot();
    if (result.at("state") == "ready")
      return result;
    if (result.at("state") == "failed")
      throw std::runtime_error(result.at("error").get<std::string>());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("Capture scan did not finish");
}
int main() {
  const auto root = fs::temp_directory_path() / ("xenon-captures-" + make_id());
  try {
    fs::create_directories(root / "first");
    fs::create_directories(root / "second");
    {
      CaptureIndex index;
      std::ofstream(root / "first" / "old.PNG") << "fixture";
      std::ofstream(root / "first" / "new.BMP") << "fixture";
      std::ofstream(root / "first" / "not-image.txt") << "fixture";
      fs::last_write_time(root / "first" / "old.PNG",
                          fs::file_time_type::clock::now() - std::chrono::hours(1));
      index.folder(root / "first");
      auto result = ready(index);
      check(result["entries"].size() == 2 && result["entries"][0]["id"] == "new.BMP",
            "Captures must include BMP, skip other files and sort newest first");
      check(result["entries"][0].at("modified").get<std::int64_t>() > 0,
            "Capture modification time must use Unix milliseconds");
      const auto version = result["entries"][0]["version"];
      std::ofstream(root / "first" / "new.BMP", std::ios::app) << "changed";
      index.refresh();
      result = ready(index);
      check(result["entries"][0]["version"] != version,
            "Refresh must invalidate an overwritten image even if the directory timestamp is "
            "unchanged");
      fs::remove(root / "first" / "old.PNG");
      index.refresh();
      check(ready(index)["entries"].size() == 1, "Rescan must remove deleted entries");
      index.folder(root / "second");
      check(index.snapshot()["entries"].empty(),
            "Changing folder must not expose captures from the previous root");
      check(ready(index)["entries"].empty(), "Empty folders must finish scanning");
      // Newest-first retention is independent of filesystem enumeration order.
      const auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(1);
      for (unsigned i = 0; i < 10003; ++i) {
        const auto file = root / "second" / (std::to_string(i) + ".png");
        std::ofstream(file) << "fixture";
        fs::last_write_time(file, old_time);
      }
      const auto latest = root / "second" / "latest.webp";
      std::ofstream(latest) << "fixture";
      index.refresh();
      result = ready(index);
      check(result["entries"].size() == 10000 && result["truncated"] == true &&
                result["entries"][0]["id"] == "latest.webp",
            "Bounded scans must retain the newest capture and report truncation");
      std::ofstream(root / "file-as-directory") << "fixture";
      index.folder(root / "file-as-directory");
      for (unsigned i = 0; i < 1000 && index.snapshot()["state"] != "failed"; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      check(index.snapshot()["state"] == "failed",
            "Unreadable folder must expose an actionable failure");
    }
    fs::remove_all(root);
    std::cout << "Capture ordering, refresh/versioning, folder changes, bounds and errors passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
