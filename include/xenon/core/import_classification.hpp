#pragma once

// Whole-XEX import capability audit classification (Phase 6 of the AC6
// Runtime Readiness pass). A small, pure, independently-testable function
// rather than logic embedded directly in tools/recomp_tools.cpp's
// import-scanner - see tests/core/import_capability_report_tests.cpp.

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

}  // namespace xenon::core
