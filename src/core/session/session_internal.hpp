#pragma once

// Private helpers shared by the XenonSession implementation files under
// src/core/session/. Not part of the public xenon/core API.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

#include "xenon/core/session.hpp"

namespace xenon::core::detail {

inline std::string ascii_lower(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

// --- Guest fault and dispatch reports (diagnostics/fault_report.cpp) -------
// Human-readable record of a guest memory fault: registers, the faulting
// access, function metadata coverage, a best-effort back-chain stack walk and
// (when tracing is on) recent export calls.
std::string describe_guest_memory_fault(const memory::MemoryFault& fault,
                                        const cpu::CpuState& state,
                                        const std::optional<xbox::LoadedXex>& loaded_xex,
                                        memory::AddressSpace& memory,
                                        const ExportTrace& export_trace,
                                        std::uint32_t thread_id);
// Message for a guest that exhausted the top-level dispatch budget.
std::string describe_dispatch_limit(const cpu::CpuState& state, const ExportTrace& export_trace,
                                    std::uint32_t thread_id);

// --- Title-specific investigation probes (diagnostics/title_probes.cpp) ----
// Ace Combat 6 probes. Each keeps its own guard, statics and output file so
// moving it out of the generic runtime changes nothing it observes.
void log_title_pm4_interrupt_handler(memory::AddressSpace& memory,
                                     gpu::GraphicsSystem& graphics_system,
                                     std::uint32_t callback_address, std::uint32_t context);
void append_title_dispatch_probe(std::ostream& out, cpu::GuestAddress next_address,
                                 const cpu::CpuState& state, cpu::ExecutionContext& context);
void log_title_thread_creation(cpu::GuestAddress start_address, std::uint64_t caller_lr,
                               std::uint64_t start_context, std::uint32_t creation_flags);
void sample_title_vsync_probes(memory::AddressSpace& memory, std::uint32_t interrupt_context);

// --- Generic runtime probes (diagnostics/runtime_probes.cpp) ---------------
// Every ~30 s (after the first 8 s), appends each known thread's recent
// export history to all_threads_export_trace_diag.log.
void snapshot_thread_export_history(const ExportTrace& export_trace);

}  // namespace xenon::core::detail
