#pragma once

// Recomp Driver entry points. This header also includes every recompiler
// analysis header it was split into, so existing includes keep working:
// discovery.hpp, module_hints.hpp, function_analysis.hpp,
// adaptive_observations.hpp, analysis_report.hpp and driver_options.hpp.
#include "xenon/recomp/cpu_coverage.hpp"
#include "xenon/recomp/compilation_graph.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "xenon/cpu/ir.hpp"
#include "xenon/recomp/analysis_schema.hpp"
#include "xenon/recomp/knowledge_base.hpp"
#include "xenon/xbox/xex_loader.hpp"
#include "xenon/recomp/adaptive_observations.hpp"
#include "xenon/recomp/analysis_report.hpp"
#include "xenon/recomp/discovery.hpp"
#include "xenon/recomp/driver_options.hpp"
#include "xenon/recomp/function_analysis.hpp"
#include "xenon/recomp/module_hints.hpp"

namespace xenon::recomp {

[[nodiscard]] bool load_and_analyze(const DriverOptions& options,
                                    AnalysisReport& report,
                                    std::string& error);
[[nodiscard]] bool generate_project(const DriverOptions& options,
                                    AnalysisReport& report,
                                    std::string& error);

}  // namespace xenon::recomp
