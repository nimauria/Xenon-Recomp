#include "core/module_settings.hpp"
#include <iostream>

using namespace xenon::launcher_next;
void check(bool success, const char *message) {
  if (!success)
    throw std::runtime_error(message);
}
template <class Operation> void rejects(Operation operation, const char *message) {
  bool failed = false;
  try {
    operation();
  } catch (const std::exception &) {
    failed = true;
  }
  check(failed, message);
}
int main() {
  try {
    const auto legacy = Json::parse(
        R"({"launcherSettings":[{"id":"enabled","type":"bool","defaultValue":true},{"id":"renderer","type":"choice","options":["Automatic","Vulkan"],"defaultValue":"Automatic"},{"id":"scale","type":"number","min":0.5,"max":2,"defaultValue":1},{"id":"quality","type":"choice","options":[{"label":"Low","value":1},{"label":"High","value":2}],"defaultValue":2}]})");
    const auto schema = module_settings_schema(legacy);
    const auto values = module_settings_values(schema, {{"enabled", false},
                                                        {"renderer", "Removed renderer"},
                                                        {"scale", 1.5},
                                                        {"quality", 1},
                                                        {"obsolete", true}});
    check(values.at("enabled") == false && values.at("scale") == 1.5 && values.at("quality") == 1,
          "Valid overrides must preserve boolean and numeric types");
    check(values.at("renderer") == "Automatic" && !values.contains("obsolete"),
          "Schema changes must fall back to declared defaults and exclude removed keys");
    rejects([&] { validate_module_setting(schema[1], "OpenGL"); },
            "Undeclared choice must be rejected");
    rejects([&] { validate_module_setting(schema[2], 2.1); },
            "Number outside module range must be rejected");
    rejects([&] { validate_module_setting(schema[0], "true"); },
            "Boolean text must not be coerced");
    rejects([&] { validate_module_setting(schema[3], "1"); }, "Numeric choices must remain typed");
    auto invalid = legacy;
    invalid["launcherSettings"].push_back(invalid["launcherSettings"][0]);
    rejects([&] { module_settings_schema(invalid); },
            "Duplicate identifiers must not create ambiguous controls");
    invalid = legacy;
    invalid["launcherSettings"][1]["defaultValue"] = "missing";
    rejects([&] { module_settings_schema(invalid); },
            "Invalid default must fail before a package is registered");
    check(module_settings_values(schema, Json::object()).at("enabled") == true,
          "Reset must restore legacy defaultValue");
    std::cout << "Legacy aliases, typed choices, bounds, defaults and schema evolution passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
