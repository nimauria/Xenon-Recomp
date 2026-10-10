#pragma once

// Tests that build a generated project against the Xenon tree, or hand the
// tree to xenon-prepare, run for minutes. Editing the working tree during
// that time can change the preparation identity or remove a source mid-build.
// These helpers give each such test a private copy of the tree instead.
//
// The copy leaves out build output, .git and the managed .xenon dependencies:
// generated projects build Xenon with SYSTEM dependencies and without
// graphics, audio or input, so they never read .xenon/deps.

#include <filesystem>
#include <string>
#include <string_view>

#if defined(XENON_SOURCE_ROOT)
namespace xenon::test {

// Copies XENON_SOURCE_ROOT to <temp>/xenon_source_<name> (replacing any
// earlier copy) and returns that path. Use a name unique to the test, since
// tests run in parallel.
inline std::filesystem::path snapshot_source_tree(std::string_view name) {
  const std::filesystem::path source(XENON_SOURCE_ROOT);
  const auto destination =
      std::filesystem::temp_directory_path() / ("xenon_source_" + std::string(name));
  std::filesystem::remove_all(destination);
  std::filesystem::create_directories(destination);
  for (const auto& entry : std::filesystem::directory_iterator(source)) {
    const auto entry_name = entry.path().filename().string();
    if (entry_name == ".git" || entry_name == ".xenon" || entry_name == "out" ||
        entry_name == "generated" || entry_name.starts_with("build"))
      continue;
    std::filesystem::copy(entry.path(), destination / entry_name,
                          std::filesystem::copy_options::recursive);
  }
  return destination;
}

}  // namespace xenon::test
#endif  // defined(XENON_SOURCE_ROOT)
