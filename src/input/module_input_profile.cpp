#include "xenon/input/module_input_profile.hpp"

#include <fstream>
#include <sstream>

#include "xenon/core/json.hpp"

namespace xenon::input {
namespace {

using xenon::core::JsonValue;

bool fail(std::string* error, std::string message) {
  if (error) *error = "input profile: " + std::move(message);
  return false;
}

bool parse_number(const JsonValue& object, std::string_view key, float min, float max,
                  float& out, const std::string& where, std::string* error) {
  const auto* value = object.find(key);
  if (!value) return true;
  if (value->type() != JsonValue::Type::Number)
    return fail(error, where + "." + std::string(key) + " must be a number");
  const auto number = value->as_number();
  if (!(number >= min && number <= max))
    return fail(error, where + "." + std::string(key) + " is out of range");
  out = static_cast<float>(number);
  return true;
}

bool parse_flag(const JsonValue& object, std::string_view key, bool& out, const std::string& where,
                std::string* error) {
  const auto* value = object.find(key);
  if (!value) return true;
  if (value->type() != JsonValue::Type::Bool)
    return fail(error, where + "." + std::string(key) + " must be true or false");
  out = value->as_bool();
  return true;
}

// [name, control] or [name, control, scale].
template <typename Source, typename Lookup>
bool parse_bindings(const JsonValue& list, const std::string& where, Lookup lookup,
                    std::vector<std::pair<Source, std::pair<VirtualControl, float>>>& out,
                    std::string* error) {
  const auto* array = list.as_array();
  if (!array) return fail(error, where + " must be an array of bindings");
  for (std::size_t i = 0; i < array->size(); ++i) {
    const auto at = where + "[" + std::to_string(i) + "]";
    const auto* binding = (*array)[i].as_array();
    if (!binding || binding->size() < 2 || binding->size() > 3 || !(*binding)[0].is_string() ||
        !(*binding)[1].is_string())
      return fail(error, at + " must be [source, control] or [source, control, scale]");
    const auto source_name = (*binding)[0].as_string();
    const auto source = lookup(source_name);
    if (!source) return fail(error, at + " names unknown input \"" + source_name + "\"");
    const auto control_name = (*binding)[1].as_string();
    const auto control = virtual_control_from_name(control_name);
    if (!control) return fail(error, at + " names unknown control \"" + control_name + "\"");
    float scale = 1.0f;
    if (binding->size() == 3) {
      if ((*binding)[2].type() != JsonValue::Type::Number)
        return fail(error, at + " scale must be a number");
      scale = static_cast<float>((*binding)[2].as_number());
      if (!(scale > 0.0f && scale <= 1.0f)) return fail(error, at + " scale must be in (0, 1]");
    }
    out.push_back({*source, {*control, scale}});
  }
  return true;
}

bool parse_context(const JsonValue& value, const std::string& where, KeyboardMouseContext& out,
                   std::string* error) {
  if (!value.is_object()) return fail(error, where + " must be an object");
  out.name = value.get_string("name");
  if (out.name.empty()) return fail(error, where + ".name must be a non-empty string");
  if (const auto* keys = value.find("keys")) {
    std::vector<std::pair<KeyCode, std::pair<VirtualControl, float>>> bindings;
    if (!parse_bindings<KeyCode>(*keys, where + ".keys",
                                 [](const std::string& n) { return key_from_name(n); }, bindings,
                                 error))
      return false;
    for (const auto& [key, control] : bindings)
      out.key_bindings.push_back({key, control.first, control.second});
  }
  if (const auto* buttons = value.find("mouseButtons")) {
    std::vector<std::pair<MouseButton, std::pair<VirtualControl, float>>> bindings;
    if (!parse_bindings<MouseButton>(*buttons, where + ".mouseButtons",
                                     [](const std::string& n) { return mouse_button_from_name(n); },
                                     bindings, error))
      return false;
    for (const auto& [button, control] : bindings)
      out.mouse_bindings.push_back({button, control.first, control.second});
  }
  if (const auto* mouse = value.find("mouse")) {
    const auto at = where + ".mouse";
    if (!mouse->is_object()) return fail(error, at + " must be an object");
    const auto stick = mouse->get_string("stick", "none");
    if (stick == "left") {
      out.mouse.stick = MouseStick::Left;
    } else if (stick == "right") {
      out.mouse.stick = MouseStick::Right;
    } else if (stick == "none") {
      out.mouse.stick = MouseStick::None;
    } else {
      return fail(error, at + ".stick must be \"left\", \"right\" or \"none\"");
    }
    if (!parse_number(*mouse, "sensitivityX", 0.01f, 100.0f, out.mouse.sensitivity_x, at, error) ||
        !parse_number(*mouse, "sensitivityY", 0.01f, 100.0f, out.mouse.sensitivity_y, at, error) ||
        !parse_number(*mouse, "smoothing", 0.0f, 1.0f, out.mouse.smoothing_seconds, at, error) ||
        !parse_number(*mouse, "antiDeadzone", 0.0f, 0.9f, out.mouse.anti_deadzone, at, error) ||
        !parse_flag(*mouse, "invertX", out.mouse.invert_x, at, error) ||
        !parse_flag(*mouse, "invertY", out.mouse.invert_y, at, error))
      return false;
    if (const auto* key = mouse->find("alternateStickKey")) {
      const auto name = key->as_string();
      const auto code = key_from_name(name);
      if (!code) return fail(error, at + ".alternateStickKey names unknown key \"" + name + "\"");
      out.mouse.alternate_stick_key = *code;
    }
  }
  return true;
}

}  // namespace

std::optional<ModuleInputProfile> parse_module_input_profile(std::string_view json,
                                                             std::string* error) {
  JsonValue root;
  std::string parse_error;
  if (!JsonValue::parse(json, root, &parse_error)) {
    fail(error, "malformed JSON: " + parse_error);
    return std::nullopt;
  }
  if (root.get_string("schema") != kModuleInputProfileSchema) {
    fail(error, "schema must be \"" + std::string(kModuleInputProfileSchema) + "\"");
    return std::nullopt;
  }
  ModuleInputProfile profile;
  if (const auto* keyboard = root.find("keyboard")) {
    if (!keyboard->is_object()) {
      fail(error, "keyboard must be an object");
      return std::nullopt;
    }
    if (const auto* contexts = keyboard->find("contexts")) {
      const auto* array = contexts->as_array();
      if (!array) {
        fail(error, "keyboard.contexts must be an array");
        return std::nullopt;
      }
      for (std::size_t i = 0; i < array->size(); ++i) {
        KeyboardMouseContext context;
        if (!parse_context((*array)[i], "keyboard.contexts[" + std::to_string(i) + "]", context,
                           error))
          return std::nullopt;
        for (const auto& existing : profile.keyboard_contexts) {
          if (existing.name == context.name) {
            fail(error, "keyboard context \"" + context.name + "\" is defined twice");
            return std::nullopt;
          }
        }
        profile.keyboard_contexts.push_back(std::move(context));
      }
    }
    profile.initial_context = keyboard->get_string("initialContext");
    if (!profile.initial_context.empty()) {
      bool known = false;
      for (const auto& context : profile.keyboard_contexts)
        known = known || context.name == profile.initial_context;
      if (!known) {
        fail(error, "keyboard.initialContext names no defined context");
        return std::nullopt;
      }
    }
  }
  return profile;
}

ModuleInputProfileLoad load_module_input_profile(const std::filesystem::path& package,
                                                 std::string_view effective_hash_hex) {
  ModuleInputProfileLoad result{};
  if (package.empty() || effective_hash_hex.empty()) return result;
  const auto path = package / "revisions" / std::string(effective_hash_hex) /
                    std::string(kModuleInputProfileFile);
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) return result;
  result.path = path;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    result.error = "input profile: cannot read " + path.string();
    return result;
  }
  std::ostringstream text;
  text << in.rdbuf();
  std::string error;
  result.profile = parse_module_input_profile(text.str(), &error);
  if (!result.profile) result.error = error + " (" + path.string() + ")";
  return result;
}

void apply_module_input_profile(const ModuleInputProfile& profile, KeyboardMouseOptions& options) {
  options.contexts = profile.keyboard_contexts;
}

}  // namespace xenon::input
