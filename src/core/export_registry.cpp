#include "xenon/core/export_registry.hpp"

#include <algorithm>
#include <cctype>
#include <mutex>

namespace xenon::core {
namespace {

std::string to_lower(std::string_view str) {
  std::string result;
  result.reserve(str.size());
  for (char c : str) {
    result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return result;
}

}  // namespace

std::string ExportRegistry::normalize_library(std::string_view library) {
  auto result = to_lower(library);
  // Remove .exe or .xex suffix if present
  if (result.ends_with(".exe")) {
    result.resize(result.size() - 4);
  } else if (result.ends_with(".xex")) {
    result.resize(result.size() - 4);
  }
  return result;
}

std::string ExportRegistry::make_ordinal_key(std::string_view library,
                                            std::uint32_t ordinal) {
  return normalize_library(library) + ":" + std::to_string(ordinal);
}

std::string ExportRegistry::make_name_key(std::string_view library,
                                         std::string_view name) {
  return normalize_library(library) + ":" + std::string(name);
}

bool ExportRegistry::register_export(ExportDescriptor descriptor) {
  if (descriptor.library.empty() || !descriptor.handler) {
    return false;
  }

  std::unique_lock lock(mutex_);

  // Register by ordinal
  auto ordinal_key = make_ordinal_key(descriptor.library, descriptor.ordinal);
  ordinal_map_[ordinal_key] = descriptor;

  // Register by name if provided
  if (!descriptor.name.empty()) {
    auto name_key = make_name_key(descriptor.library, descriptor.name);
    name_map_[name_key] = descriptor;
  }

  return true;
}

bool ExportRegistry::register_exports(std::span<const ExportDescriptor> descriptors) {
  for (const auto& desc : descriptors) {
    if (!register_export(desc)) {
      return false;
    }
  }
  return true;
}

bool ExportRegistry::unregister_export(std::string_view library,
                                      std::uint32_t ordinal) {
  std::unique_lock lock(mutex_);

  auto ordinal_key = make_ordinal_key(library, ordinal);
  auto it = ordinal_map_.find(ordinal_key);
  if (it == ordinal_map_.end()) {
    return false;
  }

  // Remove by name as well
  if (!it->second.name.empty()) {
    auto name_key = make_name_key(library, it->second.name);
    name_map_.erase(name_key);
  }

  ordinal_map_.erase(it);
  return true;
}

const ExportDescriptor* ExportRegistry::resolve(std::string_view library,
                                               std::uint32_t ordinal) const {
  std::shared_lock lock(mutex_);
  auto key = make_ordinal_key(library, ordinal);
  auto it = ordinal_map_.find(key);
  return it != ordinal_map_.end() ? &it->second : nullptr;
}

const ExportDescriptor* ExportRegistry::resolve(std::string_view library,
                                               std::string_view name) const {
  std::shared_lock lock(mutex_);
  auto key = make_name_key(library, name);
  auto it = name_map_.find(key);
  return it != name_map_.end() ? &it->second : nullptr;
}

ExportCallResult ExportRegistry::invoke(std::string_view library,
                                       std::uint32_t ordinal,
                                       ExportCallContext& context) const {
  const auto* desc = resolve(library, ordinal);
  if (!desc) {
    return {
        false,
        false,
        std::string("Unknown export: ") + std::string(library) + " ordinal " +
            std::to_string(ordinal) + " at " +
            std::to_string(context.call_address)};
  }

  bool handled = desc->handler(context);
  return {handled, handled, {}};
}

ExportCallResult ExportRegistry::invoke(std::string_view library,
                                       std::string_view name,
                                       ExportCallContext& context) const {
  const auto* desc = resolve(library, name);
  if (!desc) {
    return {false, false,
            std::string("Unknown export: ") + std::string(library) + "::" +
                std::string(name)};
  }

  bool handled = desc->handler(context);
  return {handled, handled, {}};
}

bool ExportRegistry::contains(std::string_view library,
                             std::uint32_t ordinal) const {
  return resolve(library, ordinal) != nullptr;
}

bool ExportRegistry::contains(std::string_view library,
                             std::string_view name) const {
  return resolve(library, name) != nullptr;
}

std::vector<ExportDescriptor> ExportRegistry::enumerate(
    std::string_view library) const {
  std::shared_lock lock(mutex_);
  std::vector<ExportDescriptor> result;

  if (library.empty()) {
    for (const auto& [key, desc] : ordinal_map_) {
      result.push_back(desc);
    }
  } else {
    auto normalized = normalize_library(library);
    for (const auto& [key, desc] : ordinal_map_) {
      if (normalize_library(desc.library) == normalized) {
        result.push_back(desc);
      }
    }
  }

  return result;
}

void ExportRegistry::clear() {
  std::unique_lock lock(mutex_);
  ordinal_map_.clear();
  name_map_.clear();
}

}  // namespace xenon::core
