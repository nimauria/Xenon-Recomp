#pragma once

// Whole-XEX import capability audit classification (Phase 6 of the AC6
// Runtime Readiness pass). A small, pure, independently-testable function
// rather than logic embedded directly in tools/recomp_tools.cpp's
// import-scanner - see tests/core/import_capability_report_tests.cpp.

#include <cstddef>
#include <string_view>

#include "xenon/core/export_registry.hpp"

namespace xenon::core {

enum class ImportClassification {
  Implemented,  // Registered, not a stub, not flagged partial.
  SafeStub,     // Registered as ExportRequirement::Stubbed, not partial - a
                // deliberate, safe no-op.
  Partial,      // Registered but ExportDescriptor::partial is set - a real,
                // documented behavior gap (see partial_note).
  Missing,      // Not registered at all.
};

// Classifies an import given the ExportDescriptor the production
// ExportRegistry resolved for it (nullptr if it did not resolve at all).
[[nodiscard]] ImportClassification classify_import(const ExportDescriptor* descriptor) noexcept;

[[nodiscard]] std::string_view to_string(ImportClassification classification) noexcept;

// Overall whole-XEX import capability verdict (reviewer feedback on the AC6
// Runtime Readiness pass: a binary PASS/FAIL collapses "every required
// import resolves, but some rely on a safe stub or a documented partial
// behavior gap" into the same bucket as "every import is a real, complete
// implementation" - those are not the same confidence level, and a title
// that is merely SAFE_STUB/PARTIAL-covered should not be indistinguishable
// from one with zero known gaps.
enum class ImportCapabilityVerdict {
  Pass,             // No missing required imports, and no stub/partial gaps.
  PassWithFallback, // No missing required imports, but at least one
                     // SAFE_STUB or PARTIAL import - bootable, not gap-free.
  Fail,             // At least one required import did not resolve at all.
};

// Pure aggregation over per-import classification counts - independent of
// how those counts were produced (a real XEX's import table, a synthetic
// test image, ...), so tools/recomp_tools.cpp's import-scanner and any
// other capability-report producer share one verdict rule.
[[nodiscard]] ImportCapabilityVerdict compute_import_capability_verdict(
    std::size_t implemented, std::size_t safe_stub, std::size_t partial,
    std::size_t missing) noexcept;

[[nodiscard]] std::string_view to_string(ImportCapabilityVerdict verdict) noexcept;

}  // namespace xenon::core
