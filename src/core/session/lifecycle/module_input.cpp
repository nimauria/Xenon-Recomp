#include <iostream>
#include <mutex>
#include <string>

#include "core/session/session_internal.hpp"
#include "xenon/input/module_input_profile.hpp"

namespace xenon::core {

using detail::ascii_lower;

bool XenonSession::configure_module_input() {
  if (!effective_identity_) return true;
  const auto hash =
      ascii_lower(xbox::format_effective_image_hash(effective_identity_->effective_image_hash));
  const auto load = input::load_module_input_profile(config_.module_package_path, hash);
  if (!load.error.empty()) {
    // The module ships bindings for this revision and they are wrong: running
    // without them would leave the title's controls silently unbound.
    set_error(load.error);
    return false;
  }
  if (!load.profile || !keyboard_mouse_) return true;
  auto options = keyboard_mouse_->options();
  input::apply_module_input_profile(*load.profile, options);
  keyboard_mouse_->set_options(std::move(options));
  if (!load.profile->initial_context.empty() &&
      !keyboard_mouse_->set_context(load.profile->initial_context)) {
    set_error("input profile: initial context \"" + load.profile->initial_context +
              "\" could not be selected (" + load.path.string() + ")");
    return false;
  }
  if (config_.enable_logging) {
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] Input profile: " << load.path.string() << " ("
              << load.profile->keyboard_contexts.size() << " keyboard context(s))" << std::endl;
  }
  return true;
}

std::int32_t XenonSession::module_set_input_context(void* context, const char* name) {
  auto* session = static_cast<XenonSession*>(context);
  if (!session || !session->keyboard_mouse_) return 2;
  return session->keyboard_mouse_->set_context(name ? std::string_view(name) : std::string_view{})
             ? 0
             : 1;
}

bool XenonSession::wants_mouse_capture() const {
  return keyboard_mouse_ && keyboard_mouse_->wants_mouse_capture();
}

}  // namespace xenon::core
