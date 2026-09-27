#include "content_source.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <memory>

#include "xenon/filesystem/gdfx_image_source.hpp"
#include "xenon/filesystem/path.hpp"
#include "xenon/filesystem/read_only_content_device.hpp"

namespace xenon::runtime_host {
namespace {

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string lower_extension(const std::filesystem::path& path) {
  return lower_ascii(path.extension().string());
}

bool read_file_bytes(const std::filesystem::path& path,
                     std::vector<std::byte>& out_bytes,
                     std::string& error) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    error = "could not open file: " + path.string();
    return false;
  }
  const auto size = file.tellg();
  if (size <= 0 || size > 512 * 1024 * 1024) {
    error = "file is empty or exceeds Xenon's 512 MiB executable read limit: " + path.string();
    return false;
  }
  out_bytes.resize(static_cast<std::size_t>(size));
  file.seekg(0, std::ios::beg);
  file.read(reinterpret_cast<char*>(out_bytes.data()), size);
  if (!file.good()) {
    out_bytes.clear();
    error = "could not read file completely: " + path.string();
    return false;
  }
  return true;
}

std::filesystem::path find_default_xex(const std::filesystem::path& directory) {
  std::error_code ec;
  for (std::filesystem::directory_iterator it(directory, ec), end;
       !ec && it != end; it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    if (xenon::filesystem::guest_path_equal(it->path().filename().string(), "default.xex")) {
      return it->path();
    }
  }
  return {};
}

bool load_disc_xex(const std::filesystem::path& source_path,
                   BaseContent& out,
                   std::string& error) {
  std::filesystem::path image_path;
  if (xenon::filesystem::resolve_gdfx_image_path(source_path, image_path) !=
      xenon::filesystem::FsError::None) {
    error = "unable to resolve Xbox 360 disc image: " + source_path.string();
    return false;
  }

  auto source = std::make_shared<xenon::filesystem::GdfxImageSource>(image_path);
  if (source->initialize() != xenon::filesystem::FsError::None) {
    error = "not a valid supported Xbox 360 GDFX/XDVDFS image: " + image_path.string();
    return false;
  }

  std::vector<xenon::filesystem::DirectoryEntry> entries;
  if (source->list("", entries) != xenon::filesystem::FsError::None) {
    error = "could not enumerate the Xbox 360 disc root: " + image_path.string();
    return false;
  }

  std::string executable_name;
  for (const auto& entry : entries) {
    if (!entry.info.is_directory &&
        xenon::filesystem::guest_path_equal(entry.name, "default.xex")) {
      executable_name = entry.name;
      break;
    }
  }
  if (executable_name.empty()) {
    error = "Xbox 360 disc image does not contain a root default.xex: " + image_path.string();
    return false;
  }

  if (xenon::filesystem::read_all(*source, executable_name, out.xex_bytes) !=
      xenon::filesystem::FsError::None) {
    error = "could not read default.xex from Xbox 360 disc image: " + image_path.string();
    return false;
  }

  // Mount the resolved image rather than a .dvd descriptor. GdfxImageSource
  // expects actual image bytes, and resolving once here guarantees executable
  // loading and the game:/ mount refer to the exact same backing image.
  out.mount_path = std::move(image_path);
  out.executable_description = out.mount_path.string() + ":/" + executable_name;
  return true;
}

}  // namespace

bool is_supported_base_content_path(const std::filesystem::path& path) {
  std::error_code ec;
  if (std::filesystem::is_directory(path, ec)) return true;
  if (!std::filesystem::is_regular_file(path, ec)) return false;
  const auto extension = lower_extension(path);
  return extension == ".xex" || extension == ".iso" || extension == ".xgd" ||
         extension == ".dvd";
}

bool load_base_content(const std::filesystem::path& path,
                       BaseContent& out,
                       std::string& error) {
  out = {};
  error.clear();

  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    error = "game content path does not exist: " + path.string();
    return false;
  }

  if (std::filesystem::is_directory(path, ec)) {
    const auto xex_path = find_default_xex(path);
    if (xex_path.empty()) {
      error = "could not locate a root default.xex in game directory: " + path.string();
      return false;
    }
    if (!read_file_bytes(xex_path, out.xex_bytes, error)) return false;
    out.mount_path = path;
    out.executable_description = xex_path.string();
    return true;
  }

  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    error = "game content path is neither a directory nor a supported file: " + path.string();
    return false;
  }

  const auto extension = lower_extension(path);
  if (extension == ".xex") {
    if (!read_file_bytes(path, out.xex_bytes, error)) return false;
    out.mount_path = path.parent_path();
    if (out.mount_path.empty()) out.mount_path = ".";
    out.executable_description = path.string();
    return true;
  }

  if (extension == ".iso" || extension == ".xgd" || extension == ".dvd") {
    return load_disc_xex(path, out, error);
  }

  error = "unsupported game content source (expected directory, .xex, .iso, .xgd, or .dvd): " +
          path.string();
  return false;
}

}  // namespace xenon::runtime_host
