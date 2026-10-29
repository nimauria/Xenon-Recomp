#pragma once
#include <string>
#include <vector>

namespace xenon::launcher_next {
struct Profile {
  std::string id;
  std::string name;
  std::string description;
};

// Domain logic only: transport, platform storage and CEF have no ownership
// here. IDs are supplied by the application's UUID provider, never generated
// from names.
class ProfileStore {
public:
  ProfileStore(std::vector<Profile> profiles, std::string active_id);
  void create(Profile profile);
  void activate(const std::string &id);
  void remove(const std::string &id);
  [[nodiscard]] const std::vector<Profile> &profiles() const noexcept { return profiles_; }
  [[nodiscard]] const std::string &active_id() const noexcept { return active_id_; }

private:
  std::vector<Profile> profiles_;
  std::string active_id_;
};
} // namespace xenon::launcher_next
