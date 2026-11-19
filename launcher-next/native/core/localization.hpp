#pragma once
#include "state_file.hpp"
namespace xenon::launcher_next {
// Used by native recovery surfaces before React is available. Reads the same
// editable catalogue as the renderer; malformed files always have an English fallback.
class NativeCopy {
public:
  NativeCopy(const std::filesystem::path &catalogue, const std::filesystem::path &state,
             std::string system_language = {});
  std::string text(const std::string &key, const std::string &fallback) const;
  std::string original(const std::string &english) const;
  const std::string &language() const { return language_; }

private:
  Json catalogue_ = Json::object();
  std::string language_ = "en";
};
} // namespace xenon::launcher_next
