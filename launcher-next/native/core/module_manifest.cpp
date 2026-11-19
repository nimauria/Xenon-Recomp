#include "module_manifest.hpp"
#include "file_transfer.hpp"
#include <algorithm>
namespace xenon::launcher_next {
namespace fs = std::filesystem;
namespace {
bool identifier(const std::string &value) {
  const auto alpha = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
  };
  return !value.empty() && value.size() <= 128 && alpha(value.front()) &&
         value.find("..") == std::string::npos &&
         std::all_of(value.begin(), value.end(),
                     [&](char c) { return alpha(c) || c == '.' || c == '_' || c == '-'; });
}
std::string host_platform() {
#ifdef _WIN32
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}
std::string host_architecture() {
#if defined(_M_ARM64) || defined(__aarch64__)
  return "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
  return "x64";
#else
  return "unknown";
#endif
}
} // namespace
std::string module_identity(const Json &manifest) {
  if (!manifest.is_object())
    throw std::runtime_error("Module manifest must be an object");
  const auto id = manifest.value("id", manifest.value("moduleId", ""));
  if (!identifier(id))
    throw std::runtime_error("Invalid module package identifier");
  return id;
}
Json module_runtime_apis(const Json &manifest) {
  Json source = Json::object();
  for (const auto *key : {"runtimeApis", "runtime_apis"}) {
    if (!manifest.contains(key))
      continue;
    if (!manifest.at(key).is_object())
      throw std::runtime_error("Module runtime API declarations must be an object");
    if (!manifest.at(key).empty()) {
      source = manifest.at(key);
      break;
    }
  }
  if (source.empty() && manifest.contains("runtime")) {
    const auto &runtime = manifest.at("runtime");
    if (!runtime.is_object())
      throw std::runtime_error("Module runtime declaration must be an object");
    source = runtime.value("apis", Json::object());
  }
  if (!source.is_object() || source.size() > 64)
    throw std::runtime_error("Invalid module runtime API declarations");
  Json result = Json::object();
  for (auto api = source.begin(); api != source.end(); ++api) {
    if (!identifier(api.key()) || !api.value().is_object())
      throw std::runtime_error("Invalid module runtime API declaration");
    const auto version = api.value().value("version", Json(1));
    const auto required = api.value().value("required", Json(true));
    if (!version.is_number_integer() || version.get<std::int64_t>() <= 0 ||
        version.get<std::int64_t>() > 65535 || !required.is_boolean())
      throw std::runtime_error(
          "Runtime APIs require a positive integer version and boolean required flag");
    result[api.key()] = {{"version", version}, {"required", required}};
  }
  return result;
}
Json module_compatibility(const Json &manifest, const fs::path &folder) {
  (void)module_identity(manifest);
  const auto apis = module_runtime_apis(manifest);
  const auto platform = host_platform(), architecture = host_architecture();
  Json result = {{"status", "unverified"},
                 {"runtimeApis", apis},
                 {"issues", Json::array()},
                 {"warnings", Json::array()},
                 {"platform", platform + "-" + architecture},
                 {"binaryAbiVerified", false},
                 {"nativeExtensionPath", ""}};
  for (auto api = apis.begin(); api != apis.end(); ++api) {
    if (api.key() == "input") {
      if (api.value().at("version").get<int>() > 1) {
        auto &messages = result[api.value().at("required").get<bool>() ? "issues" : "warnings"];
        messages.push_back("Input API v" + std::to_string(api.value().at("version").get<int>()) +
                           " exceeds the supported v1 contract");
      }
    } else {
      result["warnings"].push_back("The " + api.key() +
                                   " API requires validation by the Xenon adapter");
    }
  }
  if (manifest.contains("nativeExtension")) {
    auto declared = manifest.at("nativeExtension");
    if (declared.is_object()) {
      declared = declared.value(platform + "-" + architecture, declared.value(platform, Json("")));
    }
    if (!declared.is_string())
      throw std::runtime_error("Native extension must be a relative path or platform map");
    auto name = declared.get<std::string>();
    if (name.empty()) {
      result["warnings"].push_back("No native extension is declared for this platform");
    } else {
      std::replace(name.begin(), name.end(), '\\', '/');
      const auto relative = fs::u8path(name);
      if (name.size() > 4096 || name.find('\0') != std::string::npos ||
          name.find(':') != std::string::npos || relative.is_absolute() ||
          relative.has_root_path() || relative == "." ||
          std::any_of(relative.begin(), relative.end(),
                      [](const auto &part) { return part == ".."; }))
        throw std::runtime_error("Native extension must stay inside its module folder");
      const auto path = (folder / relative).lexically_normal();
      std::error_code file_error;
      if (!fs::is_regular_file(path, file_error))
        result["issues"].push_back(file_error ? "Declared native extension could not be read"
                                              : "Declared native extension is missing");
      else if (redirected_path(path) || fs::weakly_canonical(path) != path)
        result["issues"].push_back("Native extension path is redirected");
      else {
        const auto utf8 = path.u8string();
        result["nativeExtensionPath"] = std::string(utf8.begin(), utf8.end());
      }
    }
  }
  if (!result.at("issues").empty())
    result["status"] = "incompatible";
  result["message"] =
      result.at("issues").empty()
          ? "Manifest declarations checked. Binary ABI and live runtime compatibility remain "
            "unverified."
          : "Module declarations have compatibility problems. Review the listed issues.";
  return result;
}
} // namespace xenon::launcher_next
