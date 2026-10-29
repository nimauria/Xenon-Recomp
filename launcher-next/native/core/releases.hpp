#pragma once
#include "state_file.hpp"
#include <optional>
#include <vector>
namespace xenon::launcher_next {
struct Version {
  std::vector<std::uint64_t> numbers;
  std::vector<std::string> prerelease;
  std::string normalized;
  static std::optional<Version> parse(std::string text);
  int compare(const Version &other) const;
};
std::string host_key();
Json catalog_paths(const Json &document);
Json catalog_entry(const Json &source, const std::string &path,
                   const std::string &host = host_key());
Json best_release(const Json &releases, const std::string &repository, const std::string &asset,
                  bool prerelease);
bool newer_version(const std::string &candidate, const std::string &current);
std::string sha256_digest(std::string value);
} // namespace xenon::launcher_next
