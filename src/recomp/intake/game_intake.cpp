#include "xenon/recomp/game_intake.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>

#include "xenon/filesystem/gdfx_image_source.hpp"
#include "xenon/filesystem/path.hpp"
#include "xenon/filesystem/read_only_content_device.hpp"

namespace xenon::recomp {
namespace {

using xenon::filesystem::FsError;

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool read_whole_host_file(const std::filesystem::path& path, std::vector<std::byte>& out,
                          std::string& error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "unable to open file: " + path.string();
    return false;
  }
  file.seekg(0, std::ios::end);
  const auto size = file.tellg();
  if (size < 0) {
    error = "unable to determine file size: " + path.string();
    return false;
  }
  out.assign(static_cast<std::size_t>(size), std::byte{0});
  file.seekg(0, std::ios::beg);
  if (!out.empty()) {
    file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
  }
  if (!file && !file.eof()) {
    error = "failed reading file: " + path.string();
    return false;
  }
  return true;
}

// Recursively lists a GdfxImageSource looking for every ".xex" file.
// Bounded to a sane depth: GDFX images have no symlinks/cycles, but nothing
// in Gen 11 should ever walk an unbounded/pathological directory tree just
// because a container claims to have one.
bool walk_disc_for_xex(const xenon::filesystem::ReadOnlyContentSource& source,
                       const std::string& directory, int depth, bool& found_primary,
                       std::string& primary_relative, std::vector<std::string>& secondary,
                       std::string& error) {
  if (depth > 32) return true;  // give up silently past a sane depth; not a hard error

  std::vector<xenon::filesystem::DirectoryEntry> entries;
  if (source.list(directory, entries) != FsError::None) {
    if (depth == 0) {
      error = "unable to list disc image root directory";
      return false;
    }
    return true;  // an unreadable subdirectory is not fatal to the whole scan
  }

  for (const auto& entry : entries) {
    const std::string child = directory.empty() ? entry.name : directory + "/" + entry.name;
    if (entry.info.is_directory) {
      if (!walk_disc_for_xex(source, child, depth + 1, found_primary, primary_relative, secondary,
                             error)) {
        return false;
      }
      continue;
    }
    if (lower_ascii(std::filesystem::path(entry.name).extension().string()) != ".xex") continue;
    if (!found_primary && directory.empty() &&
        xenon::filesystem::guest_path_equal(entry.name, "default.xex")) {
      found_primary = true;
      primary_relative = child;
    } else {
      secondary.push_back(child);
    }
  }
  return true;
}

}  // namespace

const char* module_preparation_status_name(ModulePreparationStatus status) noexcept {
  switch (status) {
    case ModulePreparationStatus::Pending: return "Pending";
    case ModulePreparationStatus::Fresh: return "Fresh";
    case ModulePreparationStatus::Prepared: return "Prepared";
    case ModulePreparationStatus::Failed: return "Failed";
    case ModulePreparationStatus::Skipped: return "Skipped";
  }
  return "Unknown";
}

bool discover_game_executables(const std::filesystem::path& content,
                               std::vector<DiscoveredExecutable>& out, std::string& error) {
  out.clear();
  std::error_code ec;

  if (std::filesystem::is_directory(content, ec)) {
    bool found_primary = false;
    std::string primary_relative;
    std::vector<std::string> secondary;
    for (auto it = std::filesystem::recursive_directory_iterator(
             content, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      std::error_code file_ec;
      if (!it->is_regular_file(file_ec) || file_ec) continue;
      if (lower_ascii(it->path().extension().string()) != ".xex") continue;
      std::error_code relative_ec;
      const auto relative = std::filesystem::relative(it->path(), content, relative_ec);
      if (relative_ec) continue;
      const auto relative_str = relative.generic_string();
      if (!found_primary && xenon::filesystem::guest_path_equal(it->path().filename().string(),
                                                                 "default.xex")) {
        found_primary = true;
        primary_relative = relative_str;
      } else {
        secondary.push_back(relative_str);
      }
    }
    if (!found_primary) {
      error = "no default.xex found in directory: " + content.string();
      return false;
    }
    std::sort(secondary.begin(), secondary.end());
    secondary.erase(std::unique(secondary.begin(), secondary.end()), secondary.end());
    out.push_back({std::move(primary_relative), true});
    for (auto& relative_str : secondary) out.push_back({std::move(relative_str), false});
    return true;
  }

  const auto extension = lower_ascii(content.extension().string());
  if (extension == ".xex") {
    out.push_back({content.filename().generic_string(), true});
    return true;
  }

  if (extension == ".iso" || extension == ".xgd" || extension == ".dvd") {
    std::filesystem::path image_path;
    if (xenon::filesystem::resolve_gdfx_image_path(content, image_path) != FsError::None) {
      error = "unable to resolve disc image path: " + content.string();
      return false;
    }
    auto source = std::make_shared<xenon::filesystem::GdfxImageSource>(image_path);
    if (source->initialize() != FsError::None) {
      error = "not a valid Xbox 360 disc image: " + image_path.string();
      return false;
    }

    bool found_primary = false;
    std::string primary_relative;
    std::vector<std::string> secondary;
    if (!walk_disc_for_xex(*source, {}, 0, found_primary, primary_relative, secondary, error)) {
      if (error.empty()) error = "unable to list disc image root directory: " + image_path.string();
      return false;
    }
    if (!found_primary) {
      error = "disc image does not contain a root default.xex: " + image_path.string();
      return false;
    }

    std::sort(secondary.begin(), secondary.end());
    secondary.erase(std::unique(secondary.begin(), secondary.end()), secondary.end());
    out.push_back({std::move(primary_relative), true});
    for (auto& relative_str : secondary) out.push_back({std::move(relative_str), false});
    return true;
  }

  error = "unrecognized content source (expected a directory, .xex, .iso, .xgd, or .dvd): " +
          content.string();
  return false;
}

bool read_game_executable_bytes(const std::filesystem::path& content,
                                const std::string& relative_path, std::vector<std::byte>& out,
                                std::string& error) {
  std::error_code ec;
  if (std::filesystem::is_directory(content, ec)) {
    return read_whole_host_file(content / relative_path, out, error);
  }

  const auto extension = lower_ascii(content.extension().string());
  if (extension == ".xex") {
    return read_whole_host_file(content, out, error);
  }

  if (extension == ".iso" || extension == ".xgd" || extension == ".dvd") {
    std::filesystem::path image_path;
    if (xenon::filesystem::resolve_gdfx_image_path(content, image_path) != FsError::None) {
      error = "unable to resolve disc image path: " + content.string();
      return false;
    }
    auto source = std::make_shared<xenon::filesystem::GdfxImageSource>(image_path);
    if (source->initialize() != FsError::None) {
      error = "not a valid Xbox 360 disc image: " + image_path.string();
      return false;
    }
    if (xenon::filesystem::read_all(*source, relative_path, out) != FsError::None) {
      error = "failed to read '" + relative_path + "' from disc image: " + image_path.string();
      return false;
    }
    return true;
  }

  error = "unrecognized content source (expected a directory, .xex, .iso, .xgd, or .dvd): " +
          content.string();
  return false;
}

core::JsonValue GameCompilationGraph::to_json() const {
  auto root = core::JsonValue::make_object();
  root.set("schemaVersion", kSchemaVersion);
  auto modules_json = core::JsonValue::make_array();
  for (const auto& module : modules) {
    auto entry = core::JsonValue::make_object();
    entry.set("relativePath", module.relative_path);
    entry.set("isPrimary", module.is_primary);
    entry.set("status", std::string(module_preparation_status_name(module.status)));
    if (!module.title_id.empty()) entry.set("titleId", module.title_id);
    if (!module.media_id.empty()) entry.set("mediaId", module.media_id);
    if (!module.effective_image_hash.empty())
      entry.set("effectiveImageHash", module.effective_image_hash);
    if (!module.cache_key.empty()) entry.set("cacheKey", module.cache_key);
    if (!module.native_extension_path.empty())
      entry.set("nativeExtensionPath", module.native_extension_path);
    if (!module.error.empty()) entry.set("error", module.error);
    entry.set("hintsUnavailable", module.hints_unavailable);
    modules_json.append(std::move(entry));
  }
  root.set("modules", std::move(modules_json));
  return root;
}

}  // namespace xenon::recomp
