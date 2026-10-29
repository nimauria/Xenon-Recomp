#include "core/archive.hpp"
#include <fstream>
#include <iostream>
#include <random>
using namespace xenon::launcher_next;
namespace fs = std::filesystem;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  const auto root =
      fs::temp_directory_path() / ("xenon-archive-tests-" + std::to_string(std::random_device{}()));
  try {
    fs::create_directories(root);
    write_zip(root / "valid.zip",
              {{"module/module.json", "{\"id\":\"test.module\",\"version\":\"1\"}"},
               {"module/test.txt", "verified"}});
    extract_package(root / "valid.zip", root / "unpacked");
    std::ifstream input(root / "unpacked/module/test.txt");
    std::string value;
    input >> value;
    input.close();
    check(value == "verified", "Valid ZIP must round-trip");
    write_zip(root / "traversal.zip", {{"../outside.txt", "must not escape"}});
    bool rejected = false;
    try {
      extract_package(root / "traversal.zip", root / "rejected");
    } catch (...) {
      rejected = true;
    }
    check(rejected, "Traversal ZIP must be rejected");
    check(!fs::exists(root / "outside.txt"), "Traversal must not write outside staging");
    write_zip(root / "collision.zip", {{"A.txt", "first"}, {"a.txt", "second"}});
    rejected = false;
    try {
      extract_package(root / "collision.zip", root / "collision");
    } catch (...) {
      rejected = true;
    }
    check(rejected, "Case-colliding packages must be rejected");
    write_zip(root / "cancel.zip", {{"large.bin", std::string(2 * 1024 * 1024, 'x')}});
    bool cancel = false;
    rejected = false;
    try {
      extract_package(root / "cancel.zip", root / "cancelled",
                      {[&] { return cancel; },
                       [&](std::uint64_t done, std::uint64_t) {
                         if (done > 0)
                           cancel = true;
                       }});
    } catch (const OperationCancelled &) {
      rejected = true;
    }
    check(cancel && rejected, "Extraction cancellation must propagate through the ZIP callback");
    fs::remove_all(root);
    std::cout << "ZIP round-trip, traversal and cross-platform collision "
                 "checks passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
