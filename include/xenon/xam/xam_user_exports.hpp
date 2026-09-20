#pragma once

#include "xenon/core/export_registry.hpp"
#include "xenon/xam/user_manager.hpp"

namespace xenon::xam {

// Register user-related XAM exports
[[nodiscard]] bool register_user_exports(core::ExportRegistry& registry,
                                         UserManager& user_manager);

}  // namespace xenon::xam
