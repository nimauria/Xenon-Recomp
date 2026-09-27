#pragma once

#include "xenon/core/export_registry.hpp"

namespace xenon::xam {

// Register xam.xex system-information exports (XamGetSystemVersion, etc.)
[[nodiscard]] bool register_system_exports(core::ExportRegistry& registry);

}  // namespace xenon::xam
