#include "xenon/input/action_router.hpp"

namespace xenon::input {
namespace {

std::uint64_t bindingKey(FrontendInputSource source, std::int32_t code) {
  return (static_cast<std::uint64_t>(source) << 32u)
      | static_cast<std::uint32_t>(code);
}

}  // namespace

void FrontendInputRouter::bind(const FrontendInputBinding& binding) {
  bindings_[bindingKey(binding.source, binding.code)] = binding.action;
}

void FrontendInputRouter::clear() { bindings_.clear(); }

void FrontendInputRouter::dispatch(FrontendInputSource source, std::int32_t code,
                                   bool pressed, bool repeated, std::int32_t value,
                                   std::uint64_t timestamp) {
  const auto binding = bindings_.find(bindingKey(source, code));
  if (binding == bindings_.end()) return;
  push(FrontendInputEvent{source, binding->second, value, timestamp, pressed, repeated});
}

void FrontendInputRouter::push(const FrontendInputEvent& event) {
  events_.push_back(event);
}

bool FrontendInputRouter::poll(FrontendInputEvent& event) {
  if (events_.empty()) return false;
  event = events_.front();
  events_.pop_front();
  return true;
}

}  // namespace xenon::input
