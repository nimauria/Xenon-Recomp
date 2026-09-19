#pragma once

#include "xenon/input/module_api.hpp"

namespace xenon::input {
class InputSystem;
}

namespace xenon::input::module_api {

// Runtime-side owner of the public Input API table. Game modules should include
// module_api.hpp only; this provider is for Xenon runtime/frontend integration.
class Provider final {
 public:
  explicit Provider(InputSystem& system);

  [[nodiscard]] const ApiV1& api() const noexcept { return api_; }
  [[nodiscard]] const ApiV1* api_ptr() const noexcept { return &api_; }

 private:
  ApiV1 api_{};
};

}  // namespace xenon::input::module_api
