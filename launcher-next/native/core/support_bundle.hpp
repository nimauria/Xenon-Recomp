#pragma once
#include "state_file.hpp"
#include <vector>
namespace xenon::launcher_next {
std::string redact_support_text(std::string text, const std::vector<std::string> &private_values);
std::filesystem::path create_support_bundle(const std::filesystem::path &storage,
                                            const Json &state);
} // namespace xenon::launcher_next
