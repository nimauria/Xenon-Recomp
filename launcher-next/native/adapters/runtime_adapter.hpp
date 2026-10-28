#pragma once
#include "core/state_file.hpp"
#include <memory>
namespace xenon::launcher_next {
class IRuntimeAdapter {
public:
  virtual ~IRuntimeAdapter() = default;
  virtual bool available() const = 0;
  virtual Json status() = 0;
  virtual Json launch(Json configuration) = 0;
  virtual Json stop() = 0;
};
// Explicit development provider; production does not automatically discover a
// host.
std::unique_ptr<IRuntimeAdapter> fixture_runtime(std::filesystem::path executable,
                                                 std::filesystem::path storage);
} // namespace xenon::launcher_next
