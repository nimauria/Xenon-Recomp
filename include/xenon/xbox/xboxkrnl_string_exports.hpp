#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

namespace xenon::core {
class ExportRegistry;
}

namespace xenon::xbox {

// The xboxkrnl formatted-output family: sprintf/_snprintf/_scprintf/vsprintf/
// _vsnprintf/_vscprintf and their wide counterparts (ordinals 0x139-0x151),
// plus DbgPrint (0x03). All are native host code over the shared formatter in
// string_format.hpp; none needs a KernelProcess.
//
// Ordinals are taken from the xenia xboxkrnl export table (xboxkrnl_table.inc),
// not guessed. Safe to call repeatedly on the same registry.
[[nodiscard]] bool register_xboxkrnl_string_exports(xenon::core::ExportRegistry& registry);

// Kernel debug exports: DbgBreakPoint (0x01), DbgBreakPointWithStatus (0x02) and
// DbgPrompt (0x04) - there is no guest debugger on Xenon, so a break is logged and
// execution continues, and a prompt reads nothing - and KiApcNormalRoutineNop
// (0x1DF), the do-nothing APC normal routine whose address titles hand to
// KeInitializeApc. No KernelProcess needed. Safe to call repeatedly.
[[nodiscard]] bool register_xboxkrnl_debug_exports(xenon::core::ExportRegistry& registry);

// KeBugCheck (0x52) / KeBugCheckEx (0x53): a fatal, non-returning kernel stop.
// The handler is told the stop code, the four parameters (zero for the plain
// form) and a human-readable description; the owner (XenonSession) fails the
// session and requests a cooperative stop, exactly as HalReturnToFirmware does
// for a title-requested reboot. The exports return normally to their caller (the
// call itself cannot unwind the guest); the owner's cooperative stop ends the run.
struct BugCheckInfo {
  std::uint32_t code{};
  std::array<std::uint32_t, 4> parameters{};
  std::string description;
};
using BugCheckHandler = std::function<void(const BugCheckInfo&)>;
[[nodiscard]] bool register_xboxkrnl_bugcheck_exports(xenon::core::ExportRegistry& registry,
                                                      BugCheckHandler handler);

}  // namespace xenon::xbox
