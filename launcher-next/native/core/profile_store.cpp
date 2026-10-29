#include "profile_store.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace xenon::launcher_next {
namespace {
bool blank(const std::string &value) {
  return value.find_first_not_of(" \t\r\n") == std::string::npos;
}
void validate(const Profile &profile) {
  if (blank(profile.id) || blank(profile.name)) {
    throw std::invalid_argument("Profile ID and name must not be blank");
  }
}
} // namespace
ProfileStore::ProfileStore(std::vector<Profile> profiles, std::string active_id)
    : active_id_(std::move(active_id)) {
  for (auto &profile : profiles)
    create(std::move(profile));
  activate(active_id_);
}
void ProfileStore::create(Profile profile) {
  validate(profile);
  if (std::any_of(profiles_.begin(), profiles_.end(),
                  [&](const auto &p) { return p.id == profile.id; })) {
    throw std::invalid_argument("Profile ID already exists");
  }
  profiles_.push_back(std::move(profile));
}
void ProfileStore::activate(const std::string &id) {
  if (std::none_of(profiles_.begin(), profiles_.end(), [&](const auto &p) { return p.id == id; })) {
    throw std::invalid_argument("Profile does not exist");
  }
  active_id_ = id;
}
void ProfileStore::remove(const std::string &id) {
  if (active_id_ == id)
    throw std::invalid_argument("Activate another profile before removal");
  const auto found =
      std::find_if(profiles_.begin(), profiles_.end(), [&](const auto &p) { return p.id == id; });
  if (found == profiles_.end())
    throw std::invalid_argument("Profile does not exist");
  profiles_.erase(found);
}
} // namespace xenon::launcher_next
