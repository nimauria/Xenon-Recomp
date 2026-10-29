#pragma once
#include "core/state_file.hpp"
#include <atomic>
#include <memory>
#include <string>
namespace xenon::launcher_next {
// snapshot() is a nonblocking cached projection. Operations run on the native job worker.
// No Qt, runtime implementation types or guest execution cross this boundary.
class IHostServices {
public:
  virtual ~IHostServices() = default;
  virtual Json snapshot() const = 0;
  virtual Json execute(const std::string &command, const Json &data,
                       const std::shared_ptr<std::atomic_bool> &cancel) = 0;
};
std::unique_ptr<IHostServices> unavailable_host_services();
std::unique_ptr<IHostServices> fixture_host_services(const std::filesystem::path &storage);
bool host_service_command(const std::string &command);
} // namespace xenon::launcher_next
