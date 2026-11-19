#pragma once
#include "core/state_file.hpp"
#include <memory>
namespace xenon::launcher_next {
class IRuntimeAdapter {
public:
  virtual ~IRuntimeAdapter() = default;
  virtual bool available() const = 0;
  virtual bool fixture() const = 0;
  virtual const std::filesystem::path &executable() const = 0;
  virtual Json status() = 0;
  virtual Json launch(Json configuration) = 0;
  virtual Json stop() = 0;
};
// Both providers use Xenon's versioned file contract. Fixtures are opt-in and
// never become a production runtime through automatic discovery.
std::unique_ptr<IRuntimeAdapter> runtime_host(std::filesystem::path executable,
                                              std::filesystem::path storage);
std::unique_ptr<IRuntimeAdapter> fixture_runtime(std::filesystem::path executable,
                                                 std::filesystem::path storage);
} // namespace xenon::launcher_next
