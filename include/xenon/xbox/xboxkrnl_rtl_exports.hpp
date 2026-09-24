#pragma once

namespace xenon::core {
class ExportRegistry;
}

namespace xenon::xbox {

// Register Xbox 360 RTL (Runtime Library) exports with the export registry.
// This includes foundational kernel services used during executable startup
// and runtime, such as XEX header introspection, memory operations, and
// critical section management.
//
// Safe to call repeatedly on the same registry.
[[nodiscard]] bool register_xboxkrnl_rtl_exports(
    xenon::core::ExportRegistry& registry);

}  // namespace xenon::xbox
