#include <iostream>
#include <mutex>
#include <string>

#include "core/session/session_internal.hpp"
#include "xenon/gpu/temporal_profile.hpp"

namespace xenon::core {

using detail::ascii_lower;

bool XenonSession::configure_temporal_rendering() {
  if (!effective_identity_) return true;
  const auto hash =
      ascii_lower(xbox::format_effective_image_hash(effective_identity_->effective_image_hash));
  auto load = gpu::load_temporal_render_profile(config_.module_package_path, hash);
  if (!load.error.empty()) {
    // The module ships a profile for this revision and it is wrong: running
    // without it would silently drop what the module asked for.
    set_error(load.error);
    return false;
  }
  if (!graphics_system_) return true;
  auto& temporal = graphics_system_->temporal();
  temporal.set_profile(std::move(load.profile));
  if (config_.enable_logging && !load.path.empty()) {
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] Rendering profile: " << load.path.string()
              << (temporal.profile() ? " (temporal profile)" : " (no temporal section)")
              << std::endl;
  }

  const auto interval = config_.temporal_probe_interval;
  temporal.set_discovery(interval != 0u);
  if (interval == 0u) {
    temporal.set_frame_callback({});
    return true;
  }
  temporal.set_frame_callback([interval](const gpu::TemporalFrameReport& report) {
    if (report.frame % interval != 0u) return;
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[TemporalProbe] " << gpu::format_temporal_frame_report(report) << std::flush;
  });
  if (config_.enable_logging) {
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] Temporal probe: every " << interval << " frames"
              << (temporal.profile() ? "" : " (discovery only: no temporal profile for revision " +
                                                hash + ")")
              << std::endl;
  }
  return true;
}

}  // namespace xenon::core
