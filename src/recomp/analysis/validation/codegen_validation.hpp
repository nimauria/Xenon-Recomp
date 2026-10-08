#pragma once

// Pre-emission validation of a finished analysis: the invariants code
// generation relies on (one guest address -> one canonical function -> one
// emitted symbol, every non-local edge lands on a dispatchable entry).

#include <string>
#include <vector>

#include "xenon/recomp/driver.hpp"

namespace xenon::recomp::detail {

bool validate_codegen_uniqueness(const std::vector<const DiscoveredFunction*>& codegen_items,
                                 const AnalysisReport& report, AnalysisDiagnostics& diagnostics,
                                 std::string& error);
bool validate_codegen_control_flow(const std::vector<const DiscoveredFunction*>& functions,
                                   const AnalysisReport& report, std::string& error);
bool validate_region_entry_integrity(const AnalysisReport& report,
                                     AnalysisDiagnostics& diagnostics,
                                     std::string& error);

}  // namespace xenon::recomp::detail
