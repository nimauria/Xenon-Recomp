#pragma once

namespace xenon::core {
class ExportRegistry;
}

namespace xenon::xbox {

// Registers the xboxkrnl guest timebase/timing exports this pass adds:
// KeQueryPerformanceFrequency, KeQuerySystemTime, KeDelayExecutionThread,
// KeStallExecutionProcessor. Ordinals were verified against the
// xenia-project/xenia xboxkrnl export table (see docs/kernel/THREADING_V2.md)
// rather than guessed, per this project's own hard-learned lesson about
// plausible-but-wrong ordinals (RtlImageXexHeaderField / 0x12B).
//
// Deliberately does not register KeQueryPerformanceCounter or
// NtDelayExecution: neither exists as a real xboxkrnl export on Xbox 360 (no
// entry in the reference ordinal table). A real title reads the performance
// counter via the PPC time-base register directly (see
// XenonSession::read_time_base(), already wired through RuntimeServices),
// and delays execution via KeDelayExecutionThread.
//
// Safe to call repeatedly on the same registry. No session dependency: every
// handler here only touches TimeServices and the calling guest thread's own
// registers/memory, so it registers directly into the registry rather than
// needing a XenonSession-owned bridge.
[[nodiscard]] bool register_xboxkrnl_time_exports(
    xenon::core::ExportRegistry& registry);

}  // namespace xenon::xbox
