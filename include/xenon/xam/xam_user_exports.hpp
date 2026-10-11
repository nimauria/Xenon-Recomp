#pragma once

#include "xenon/core/export_registry.hpp"
#include "xenon/xam/user_manager.hpp"

namespace xenon::core {
class XenonSession;
}

namespace xenon::xam {

// Register user-related XAM exports. `session` completes the XOVERLAPPED
// calls take (see src/xam/xam_overlapped.hpp).
[[nodiscard]] bool register_user_exports(core::ExportRegistry& registry,
                                         UserManager& user_manager,
                                         core::XenonSession& session);

}  // namespace xenon::xam
