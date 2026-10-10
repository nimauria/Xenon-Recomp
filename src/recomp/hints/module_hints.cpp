#include <exception>
#include <fstream>
#include <sstream>

#include "recomp/driver/driver_support.hpp"

namespace xenon::recomp {
namespace detail {
namespace {

bool parse_address_list(const std::string& value, std::vector<std::uint32_t>& output) {
  std::istringstream input(value);
  std::string token;
  while (std::getline(input, token, ',')) {
    try {
      output.push_back(static_cast<std::uint32_t>(std::stoul(token, nullptr, 0)));
    } catch (const std::exception&) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool load_hints(const DriverOptions& options, std::vector<ModuleHint>& hints, std::string& error) {
  hints = options.hints;
  if (options.hints_file.empty()) return true;
  std::ifstream input(options.hints_file);
  if (!input) {
    error = "unable to open module hints: " + options.hints_file.string();
    return false;
  }
  ModuleHint hint{};
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto separator = line.find('=');
    if (separator == std::string::npos) {
      error = "invalid module hint line: " + line;
      return false;
    }
    const auto key = line.substr(0, separator);
    const auto value = line.substr(separator + 1);
    if (key == "name") hint.name = value;
    else if (key == "function_boundaries" && !parse_address_list(value, hint.function_boundaries))
      error = "invalid function_boundaries hint";
    else if (key == "data_regions" && !parse_address_list(value, hint.data_regions))
      error = "invalid data_regions hint";
    else if (key == "ignored_regions" && !parse_address_list(value, hint.ignored_regions))
      error = "invalid ignored_regions hint";
    else if (key == "known_symbols") {
      const auto at = value.find('@');
      if (at == std::string::npos) {
        error = "known_symbols must use name@address";
      } else {
        try {
          hint.known_symbols.push_back({static_cast<std::uint32_t>(
              std::stoul(value.substr(at + 1), nullptr, 0)), value.substr(0, at)});
        } catch (const std::exception&) {
          error = "invalid known_symbols hint";
        }
      }
    }
    else if (key == "special_hooks") hint.special_hooks.push_back(value);
    else if (key == "patches") hint.patches.push_back(value);
    else {
      error = "unknown module hint key: " + key;
    }
    if (!error.empty()) return false;
  }
  if (!hint.name.empty() || !hint.function_boundaries.empty() || !hint.known_symbols.empty())
    hints.push_back(std::move(hint));
  return true;
}

}  // namespace detail

void ModuleCatalog::register_provider(const ModuleHintProvider& provider) {
  providers_.push_back(&provider);
}

}  // namespace xenon::recomp
