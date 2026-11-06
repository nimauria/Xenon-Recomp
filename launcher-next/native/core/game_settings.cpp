#include "game_settings.hpp"
#include "module_settings.hpp"
#include "settings.hpp"
#include "catalogues.hpp"
#include <algorithm>
namespace xenon::launcher_next {
Json game_runtime_values(const Json &values) {
  if (!values.is_object())
    throw std::runtime_error("Game settings must be an object");
  const auto &definitions = profile_runtime_definitions();
  for (auto field = values.begin(); field != values.end(); ++field) {
    if (std::none_of(definitions.begin(), definitions.end(),
                     [&](const auto &definition) { return definition.at("key") == field.key(); }))
      throw std::runtime_error("Unsupported game setting");
    validate_setting(field.key(), field.value());
  }
  return values;
}
Json effective_game_settings(const Json &game, const Json &profile, const Json &global) {
  auto result = effective_profile_settings(profile, global);
  const auto values = game_runtime_values(game.value("runtimeOverrides", Json::object()));
  for (auto field = values.begin(); field != values.end(); ++field)
    result[field.key()] = field.value();
  return result;
}
Json game_module_values(const Json &schema, const Json &values) {
  if (!values.is_object())
    throw std::runtime_error("Game module settings must be an object");
  for (auto field = values.begin(); field != values.end(); ++field) {
    const auto found = std::find_if(schema.begin(), schema.end(), [&](const auto &definition) {
      return definition.at("id") == field.key();
    });
    if (found == schema.end())
      throw std::runtime_error("This game option is no longer available; reset the game options");
    validate_module_setting(*found, field.value());
  }
  return values;
}
Json effective_game_module_settings(const Json &game, const Json &module) {
  auto result = module_settings_values(module.at("schema"), module.at("settings"));
  const auto overrides = game.value("moduleOverrides", Json::object());
  if (!overrides.is_object())
    throw std::runtime_error("Game module settings must be an object");
  const auto values = game_module_values(
      module.at("schema"), overrides.value(module.at("id").get<std::string>(), Json::object()));
  for (auto field = values.begin(); field != values.end(); ++field)
    result[field.key()] = field.value();
  return result;
}
} // namespace xenon::launcher_next
