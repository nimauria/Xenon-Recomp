#include "module_settings.hpp"
#include <cmath>
#include <set>

namespace xenon::launcher_next {
namespace {
bool scalar(const Json &value) {
  return value.is_string() || value.is_boolean() ||
         (value.is_number() && std::isfinite(value.get<double>()));
}
bool safe_id(const std::string &value) {
  const auto alpha = [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
  };
  if (value.empty() || value.size() > 128 || !alpha(value.front()))
    return false;
  for (const auto c : value)
    if (!alpha(c) && c != '.' && c != '_' && c != '-')
      return false;
  return true;
}
} // namespace
void validate_module_setting(const Json &definition, const Json &value) {
  const auto type = definition.at("type").get<std::string>();
  if (type == "bool") {
    if (!value.is_boolean())
      throw std::runtime_error("This module setting requires a boolean");
  } else if (type == "number") {
    if (!value.is_number() || !std::isfinite(value.get<double>()))
      throw std::runtime_error("This module setting requires a finite number");
    const auto number = value.get<double>();
    if ((definition.contains("minimum") && number < definition.at("minimum").get<double>()) ||
        (definition.contains("maximum") && number > definition.at("maximum").get<double>()))
      throw std::runtime_error("Module setting is outside its allowed range");
  } else if (type == "string") {
    if (!value.is_string())
      throw std::runtime_error("This module setting requires text");
  } else if (type == "choice") {
    bool allowed = false;
    for (const auto &option : definition.at("options"))
      if ((option.is_object() ? option.at("value") : option) == value)
        allowed = true;
    if (!allowed)
      throw std::runtime_error("This value is not allowed by the module manifest");
  } else
    throw std::runtime_error("Unsupported module setting type");
}
Json module_settings_schema(const Json &manifest) {
  auto source = manifest.value("settings", Json::array());
  if (!source.is_array())
    throw std::runtime_error("Module settings must be an array");
  if (source.empty())
    source = manifest.value("launcherSettings", Json::array());
  if (!source.is_array() || source.size() > 512)
    throw std::runtime_error("Invalid module settings schema");
  Json result = Json::array();
  std::set<std::string> ids;
  for (auto definition : source) {
    if (!definition.is_object() || !definition.contains("id") || !definition.at("id").is_string())
      throw std::runtime_error("Module setting has no identifier");
    const auto id = definition.at("id").get<std::string>();
    if (!safe_id(id) || !ids.insert(id).second)
      throw std::runtime_error("Invalid or duplicate module setting identifier");
    auto type = definition.value("type", "string");
    for (auto &c : type)
      if (c >= 'A' && c <= 'Z')
        c += 'a' - 'A';
    if (type == "boolean")
      type = "bool";
    if (type == "enum")
      type = "choice";
    if (type != "bool" && type != "number" && type != "string" && type != "choice")
      throw std::runtime_error("Unsupported module setting type: " + type);
    definition["type"] = type;
    if (!definition.contains("label"))
      definition["label"] = id;
    if (!definition.at("label").is_string() ||
        (definition.contains("description") && !definition.at("description").is_string()))
      throw std::runtime_error("Invalid module setting label");
    for (const auto *key : {"minimum", "maximum"}) {
      const auto alias = std::string(key) == "minimum" ? "min" : "max";
      if (!definition.contains(key) && definition.contains(alias))
        definition[key] = definition[alias];
      if (definition.contains(key) &&
          (!definition[key].is_number() || !std::isfinite(definition[key].get<double>())))
        throw std::runtime_error("Invalid module setting range");
    }
    if (definition.contains("minimum") && definition.contains("maximum") &&
        definition["minimum"].get<double>() > definition["maximum"].get<double>())
      throw std::runtime_error("Invalid module setting range");
    if (type == "choice") {
      if (!definition.contains("options") || !definition["options"].is_array() ||
          definition["options"].empty() || definition["options"].size() > 512)
        throw std::runtime_error("Module choice requires options");
      std::set<std::string> seen;
      for (const auto &option : definition["options"]) {
        if (option.is_object() && (!option.contains("label") || !option.at("label").is_string() ||
                                   !option.contains("value")))
          throw std::runtime_error("Invalid module choice option");
        const auto &value = option.is_object() ? option.at("value") : option;
        if (!scalar(value) || !seen.insert(value.dump()).second)
          throw std::runtime_error("Invalid or duplicate module choice option");
      }
    }
    if (!definition.contains("default")) {
      if (definition.contains("defaultValue"))
        definition["default"] = definition["defaultValue"];
      else if (type == "bool")
        definition["default"] = false;
      else if (type == "number")
        definition["default"] = definition.value("minimum", 0.);
      else if (type == "choice") {
        const auto &option = definition["options"][0];
        definition["default"] = option.is_object() ? option.at("value") : option;
      } else
        definition["default"] = "";
    }
    validate_module_setting(definition, definition.at("default"));
    result.push_back(std::move(definition));
  }
  return result;
}
Json module_settings_values(const Json &schema, const Json &overrides) {
  Json values = Json::object();
  for (const auto &definition : schema) {
    const auto id = definition.at("id").get<std::string>();
    values[id] = definition.at("default");
    if (overrides.contains(id))
      try {
        validate_module_setting(definition, overrides.at(id));
        values[id] = overrides.at(id);
      } catch (const std::exception &) {
      }
  }
  return values;
}
} // namespace xenon::launcher_next
