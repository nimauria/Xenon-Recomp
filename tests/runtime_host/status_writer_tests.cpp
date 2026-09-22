// Unit tests for xenon::runtime_host::StatusWriter's structured launch-
// failure reporting (Part 5.7 of the Gracemeria readiness pass): write_fatal()
// must publish an "errorCategory" field a launcher/dashboard can branch on,
// distinct from the free-text "lastError" message, and must omit it when no
// category applies (back-compat with callers that only pass a message).

#include "status_writer.hpp"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "launch_config.hpp"
#include "xenon/core/json.hpp"

namespace {

std::string read_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << file.rdbuf();
  return buffer.str();
}

}  // namespace

int main() {
  std::cout << "Testing Xenon Runtime Host StatusWriter...\n";

  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto session_dir =
      std::filesystem::temp_directory_path() / ("xenon_status_writer_test_" + std::to_string(stamp));
  std::filesystem::remove_all(session_dir);
  std::filesystem::create_directories(session_dir);

  xenon::runtime_host::LaunchConfig launch{};
  launch.session_id = "session-status-test";
  launch.game_id = "test-game";

  // Test 1: write_fatal() with an explicit category publishes it.
  {
    xenon::runtime_host::StatusWriter status(session_dir.string(), launch);
    status.write_fatal("Required base content could not be mounted",
                       xenon::runtime_host::LaunchFailureCategory::ContentMountFailed);

    xenon::core::JsonValue root;
    std::string parse_error;
    const auto contents = read_file(std::filesystem::path(status.status_path()));
    assert(!contents.empty() && "status.json should have been written");
    assert(xenon::core::JsonValue::parse(contents, root, &parse_error) && "status.json must be valid JSON");
    assert(root.get_string("lastError") == "Required base content could not be mounted");
    assert(root.get_string("errorCategory") == "ContentMountFailed");
    assert(root.get_string("stateName") == "failed");
  }
  std::cout << "  [PASS] write_fatal() publishes an explicit errorCategory\n";

  // Test 2: write_fatal() with no category (the default) omits the field
  // entirely rather than publishing a misleading "None" string - preserves
  // the original single-argument call sites that predate categorization.
  {
    xenon::runtime_host::StatusWriter status(session_dir.string(), launch);
    status.write_fatal("Something went wrong");

    xenon::core::JsonValue root;
    std::string parse_error;
    const auto contents = read_file(std::filesystem::path(status.status_path()));
    assert(xenon::core::JsonValue::parse(contents, root, &parse_error));
    assert(root.get_string("lastError") == "Something went wrong");
    assert(root.find("errorCategory") == nullptr &&
           "errorCategory must be omitted when no category is given");
  }
  std::cout << "  [PASS] write_fatal() omits errorCategory when uncategorized\n";

  // Test 3: every category name is distinct and non-empty (guards against a
  // future enum entry silently falling through to the same default string).
  {
    const xenon::runtime_host::LaunchFailureCategory categories[] = {
        xenon::runtime_host::LaunchFailureCategory::None,
        xenon::runtime_host::LaunchFailureCategory::SessionInitFailed,
        xenon::runtime_host::LaunchFailureCategory::MissingRequiredContent,
        xenon::runtime_host::LaunchFailureCategory::ContentMountFailed,
        xenon::runtime_host::LaunchFailureCategory::GameLoadFailed,
        xenon::runtime_host::LaunchFailureCategory::ModuleRevisionMismatch,
        xenon::runtime_host::LaunchFailureCategory::StartFailed,
        xenon::runtime_host::LaunchFailureCategory::WindowCreationFailed,
        xenon::runtime_host::LaunchFailureCategory::GraphicsPresentationFailed,
        xenon::runtime_host::LaunchFailureCategory::AudioBackendFailed,
        xenon::runtime_host::LaunchFailureCategory::InputBackendFailed,
    };
    for (std::size_t i = 0; i < std::size(categories); ++i) {
      const std::string name_i = xenon::runtime_host::launch_failure_category_name(categories[i]);
      assert(!name_i.empty());
      for (std::size_t j = i + 1; j < std::size(categories); ++j) {
        assert(name_i != xenon::runtime_host::launch_failure_category_name(categories[j]) &&
               "every LaunchFailureCategory must have a distinct name");
      }
    }
  }
  std::cout << "  [PASS] Every LaunchFailureCategory has a distinct, non-empty name\n";

  std::filesystem::remove_all(session_dir);
  std::cout << "All StatusWriter tests passed!\n";
  return 0;
}
