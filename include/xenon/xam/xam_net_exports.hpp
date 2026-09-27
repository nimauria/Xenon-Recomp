#pragma once

#include "xenon/core/export_registry.hpp"

namespace xenon::xam {

// Register xam.xex NetDll_* (XNet/Winsock guest ABI) exports. These are the
// generic Xbox 360 network-stack entry points every title links against
// through xam.xex - distinct from Xenon Network (docs/network/
// XENON_NETWORK_V1.md), which is a separate, unrelated future online
// service. Real hardware initializes this local XNet/Winsock state
// successfully offline (no cable, no Live signin required); only later
// calls that need an actual connection observe "not connected".
[[nodiscard]] bool register_net_exports(core::ExportRegistry& registry);

}  // namespace xenon::xam
