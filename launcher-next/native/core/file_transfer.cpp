#include "file_transfer.hpp"
#include "core/archive.hpp"
#include "picosha2/picosha2.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <vector>
#include <set>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
namespace xenon::launcher_next {
namespace fs = std::filesystem;
namespace {
constexpr std::uint64_t maximum_bytes = 512ull * 1024 * 1024 * 1024;
std::string text(const fs::path &path) {
  const auto value = path.generic_u8string();
  return {value.begin(), value.end()};
}
std::string stamp(const fs::path &path) {
  return std::to_string(fs::last_write_time(path).time_since_epoch().count());
}
void plain(const fs::path &path) {
  if (redirected_path(path) || (!fs::is_directory(path) && !fs::is_regular_file(path)))
    throw std::runtime_error("Content contains a link or unsupported filesystem entry");
}
std::string digest(const fs::path &path, const std::function<void()> &check) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream)
    throw std::runtime_error("Cannot read content for verification");
  picosha2::hash256_one_by_one hash;
  std::array<char, 256 * 1024> buffer{};
  while (stream) {
    check();
    stream.read(buffer.data(), buffer.size());
    hash.process(buffer.data(), buffer.data() + stream.gcount());
  }
  if (!stream.eof())
    throw std::runtime_error("Content could not be verified");
  hash.finish();
  return picosha2::get_hash_hex_string(hash);
}
fs::path relative_path(const nlohmann::json &entry) {
  const auto path = fs::u8path(entry.at("relative").get<std::string>());
  if (path.is_absolute() || path.has_root_name() || path.empty() || path != path.lexically_normal())
    throw std::runtime_error("Invalid content manifest path");
  for (const auto &part : path)
    if (part == ".." || part == ".")
      throw std::runtime_error("Invalid content manifest path");
  return path;
}
} // namespace
bool redirected_path(const fs::path &path) {
  if (fs::is_symlink(fs::symlink_status(path)))
    return true;
#ifdef _WIN32
  const auto attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES)
    throw std::runtime_error("Cannot inspect content attributes");
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  return false;
#endif
}
bool path_within(const fs::path &path, const fs::path &root) {
  if (!path.is_absolute() || !root.is_absolute())
    return false;
  const auto candidate = path.lexically_normal(), boundary = root.lexically_normal();
  auto part = candidate.begin();
  for (const auto &expected : boundary) {
    if (expected.empty())
      continue;
    if (part == candidate.end())
      return false;
#ifdef _WIN32
    if (CompareStringOrdinal(part->c_str(), -1, expected.c_str(), -1, TRUE) != CSTR_EQUAL)
      return false;
#else
    if (*part != expected)
      return false;
#endif
    ++part;
  }
  return true;
}
void check_transfer_tree(const fs::path &path) {
  plain(path);
  std::size_t count = 0;
  if (fs::is_directory(path))
    for (const auto &entry : fs::recursive_directory_iterator(path)) {
      if (++count > 50010)
        throw std::runtime_error("Content exceeds 50,000 entries");
      plain(entry.path());
    }
}
nlohmann::json
copy_library_content(const fs::path &source, const fs::path &destination,
                     const std::function<void()> &check,
                     const std::function<void(std::uint64_t, std::uint64_t)> &progress) {
  plain(source);
  const bool folder = fs::is_directory(source);
  const auto base = folder ? source : source.parent_path();
  nlohmann::json manifest = {{"folder", folder},
                             {"files", nlohmann::json::array()},
                             {"directories", nlohmann::json::array()}};
  std::uint64_t total = 0, done = 0;
  std::size_t count = 0;
  const auto inspect = [&](const fs::path &path) {
    check();
    plain(path);
    if (++count > 50000)
      throw std::runtime_error("Content exceeds 50,000 entries");
    const auto relative = path.lexically_relative(base);
    if (std::distance(relative.begin(), relative.end()) > 64)
      throw std::runtime_error("Content exceeds the directory depth limit");
    if (fs::is_directory(path))
      manifest["directories"].push_back(text(relative));
    else {
      const auto size = fs::file_size(path);
      if (size > maximum_bytes - total)
        throw std::runtime_error("Content exceeds 512 GiB");
      total += size;
      manifest["files"].push_back(
          {{"relative", text(relative)}, {"size", size}, {"modified", stamp(path)}});
    }
  };
  if (folder)
    for (const auto &entry : fs::recursive_directory_iterator(source))
      inspect(entry.path());
  else
    inspect(source);
  if (!fs::create_directory(destination))
    throw std::runtime_error("Content staging must be a new directory");
  for (const auto &directory : manifest["directories"])
    fs::create_directories(destination / fs::u8path(directory.get<std::string>()));
  std::array<char, 256 * 1024> buffer{};
  for (auto &entry : manifest["files"]) {
    check();
    const auto relative = relative_path(entry), input_path = base / relative,
               output_path = destination / relative;
    if (fs::weakly_canonical(input_path) != input_path || redirected_path(input_path))
      throw std::runtime_error("The source was redirected during transfer");
    std::ifstream input(input_path, std::ios::binary);
    std::ofstream output(output_path, std::ios::binary);
    if (!input || !output)
      throw std::runtime_error("Cannot copy content");
    picosha2::hash256_one_by_one hash;
    std::uint64_t copied = 0;
    while (input) {
      check();
      input.read(buffer.data(), buffer.size());
      const auto size = input.gcount();
      if (size > 0) {
        output.write(buffer.data(), size);
        if (!output)
          throw std::runtime_error("Could not write managed content");
        hash.process(buffer.data(), buffer.data() + size);
        copied += size;
        done += size;
        progress(done, total);
      }
    }
    output.flush();
    output.close();
    if (!input.eof() || !output || copied != entry.at("size").get<std::uint64_t>() ||
        stamp(input_path) != entry.at("modified").get<std::string>())
      throw std::runtime_error("Source changed during transfer; original files were preserved");
    hash.finish();
    entry["sha256"] = picosha2::get_hash_hex_string(hash);
    if (digest(output_path, check) != entry.at("sha256").get<std::string>() ||
        digest(input_path, check) != entry.at("sha256").get<std::string>())
      throw std::runtime_error("Content verification failed; original files were preserved");
  }
  return manifest;
}
nlohmann::json
copy_library_sources(const std::vector<fs::path> &sources, const fs::path &destination,
                     const std::function<void()> &check,
                     const std::function<void(std::uint64_t, std::uint64_t)> &progress) {
  if (sources.empty() || sources.size() > 128)
    throw std::runtime_error("Choose between one and 128 add-on sources");
  if (sources.size() == 1)
    return copy_library_content(sources.front(), destination, check, progress);
  std::uint64_t total = 0;
  std::size_t count = 0;
  std::set<fs::path> names;
  for (const auto &source : sources) {
    if (!names.insert(source.filename()).second)
      throw std::runtime_error("Selected add-on sources have conflicting names");
    const auto inspect = [&](const fs::path &path) {
      check();
      plain(path);
      if (++count > 50000)
        throw std::runtime_error("Content exceeds 50,000 entries");
      if (fs::is_regular_file(path)) {
        const auto size = fs::file_size(path);
        if (size > maximum_bytes - total)
          throw std::runtime_error("Content exceeds 512 GiB");
        total += size;
      }
    };
    inspect(source);
    if (fs::is_directory(source))
      for (const auto &entry : fs::recursive_directory_iterator(source))
        inspect(entry.path());
  }
  if (!fs::create_directory(destination))
    throw std::runtime_error("Content staging must be a new directory");
  nlohmann::json result = {{"folder", true},
                           {"files", nlohmann::json::array()},
                           {"directories", nlohmann::json::array()}};
  std::uint64_t completed = 0;
  for (std::size_t i = 0; i < sources.size(); ++i) {
    const auto &source = sources[i];
    const auto part = destination.parent_path() / ("source-part-" + std::to_string(i));
    const auto manifest = copy_library_content(
        source, part, check, [&](auto done, auto) { progress(completed + done, total); });
    const bool folder = manifest.at("folder").get<bool>();
    const auto prefix = folder ? source.filename() : fs::path{};
    const auto target = destination / source.filename();
    if (fs::exists(target))
      throw std::runtime_error("Selected add-on sources collide in managed storage");
    if (folder) {
      fs::rename(part, target);
      result["directories"].push_back(text(prefix));
    } else {
      fs::rename(part / source.filename(), target);
      fs::remove(part);
    }
    for (auto entry : manifest.at("files")) {
      if (folder)
        entry["relative"] = text(prefix / fs::u8path(entry.at("relative").get<std::string>()));
      completed += entry.at("size").get<std::uint64_t>();
      result["files"].push_back(entry);
    }
    for (const auto &directory : manifest.at("directories"))
      result["directories"].push_back(text(prefix / fs::u8path(directory.get<std::string>())));
  }
  return result;
}
void verify_library_content(const fs::path &destination, const nlohmann::json &manifest,
                            const std::function<void()> &check) {
  if (fs::weakly_canonical(destination) != destination)
    throw std::runtime_error("Managed content was redirected");
  check_transfer_tree(destination);
  std::set<std::string> expected, actual;
  for (const auto &entry : manifest.at("files")) {
    check();
    const auto relative = relative_path(entry), file = destination / relative;
    if (!expected.insert(text(relative)).second || !fs::is_regular_file(file) ||
        fs::file_size(file) != entry.at("size").get<std::uint64_t>() ||
        digest(file, check) != entry.at("sha256").get<std::string>())
      throw std::runtime_error("Managed content no longer matches its import receipt");
  }
  for (const auto &entry : fs::recursive_directory_iterator(destination)) {
    check();
    if (entry.is_regular_file())
      actual.insert(text(entry.path().lexically_relative(destination)));
  }
  if (actual != expected)
    throw std::runtime_error("Managed content contains files absent from its import receipt");
}
bool remove_transferred_source(const fs::path &source, const fs::path &destination,
                               const nlohmann::json &manifest, const std::function<void()> &check) {
  const auto base = manifest.at("folder").get<bool>() ? source : source.parent_path();
  for (const auto &entry : manifest.at("files"))
    (void)relative_path(entry);
  for (const auto &directory : manifest.at("directories"))
    (void)relative_path(nlohmann::json{{"relative", directory}});
  bool retained = false;
  // Never recurse-delete the source. Only verified, unchanged entries may be removed.
  for (const auto &entry : manifest.at("files")) {
    check();
    const auto relative = relative_path(entry), input = base / relative,
               output = destination / relative;
    std::error_code error;
    if (!fs::exists(input, error) && !error)
      continue;
    try {
      if (fs::weakly_canonical(input) != input || fs::weakly_canonical(output) != output ||
          redirected_path(input) || redirected_path(output) ||
          fs::file_size(input) != entry.at("size").get<std::uint64_t>() ||
          stamp(input) != entry.at("modified").get<std::string>() ||
          digest(output, check) != entry.at("sha256").get<std::string>() ||
          digest(input, check) != entry.at("sha256").get<std::string>()) {
        retained = true;
        continue;
      }
      if (stamp(input) != entry.at("modified").get<std::string>() ||
          fs::file_size(input) != entry.at("size").get<std::uint64_t>() ||
          !fs::remove(input, error) || error)
        retained = true;
    } catch (const OperationCancelled &) {
      throw;
    } catch (const std::exception &) {
      retained = true;
    }
  }
  if (manifest.at("folder").get<bool>()) {
    auto directories = manifest.at("directories").get<std::vector<std::string>>();
    std::sort(directories.begin(), directories.end(),
              [](const auto &a, const auto &b) { return a.size() > b.size(); });
    directories.push_back("");
    for (const auto &relative : directories) {
      check();
      const auto path = relative.empty() ? source : source / fs::u8path(relative);
      std::error_code error;
      if (fs::exists(path, error)) {
        if (fs::weakly_canonical(path) != path || redirected_path(path) ||
            !fs::is_empty(path, error) || error)
          retained = true;
        else if (!fs::remove(path, error) || error)
          retained = true;
      }
    }
  }
  return !retained;
}
} // namespace xenon::launcher_next
