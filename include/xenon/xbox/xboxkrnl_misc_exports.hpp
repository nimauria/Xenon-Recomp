#pragma once

#include "xenon/core/export_registry.hpp"

namespace xenon::xbox {

// Register xboxkrnl.exe exports that need no KernelProcess/session state
// (XeCryptSha, ExGetXConfigSetting, ExRegisterTitleTerminateNotification).
[[nodiscard]] bool register_xboxkrnl_misc_exports(core::ExportRegistry& registry);

}  // namespace xenon::xbox
