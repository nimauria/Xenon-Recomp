#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>

#include "core/session/session_internal.hpp"

namespace xenon::core::detail {

std::string describe_guest_memory_fault(const memory::MemoryFault& fault,
                                        const cpu::CpuState& state,
                                        const std::optional<xbox::LoadedXex>& loaded_xex,
                                        memory::AddressSpace& memory,
                                        const ExportTrace& export_trace,
                                        std::uint32_t thread_id) {
  const auto& info = fault.info();
  const auto access_name = [](memory::AccessKind access) {
    switch (access) {
      case memory::AccessKind::Read: return "read";
      case memory::AccessKind::Write: return "write";
      case memory::AccessKind::Execute: return "execute";
    }
    return "unknown";
  };
  const auto reason_name = [](memory::FaultReason reason) {
    switch (reason) {
      case memory::FaultReason::Unmapped: return "unmapped";
      case memory::FaultReason::Uncommitted: return "uncommitted";
      case memory::FaultReason::Protection: return "protection";
      case memory::FaultReason::OutOfRange: return "out-of-range";
      case memory::FaultReason::MmioWidth: return "mmio-width";
    }
    return "unknown";
  };
  const auto page_state_name = [](memory::PageState page_state) {
    switch (page_state) {
      case memory::PageState::Free: return "free";
      case memory::PageState::Reserved: return "reserved";
      case memory::PageState::Committed: return "committed";
    }
    return "unknown";
  };
  const auto protect_string = [](memory::Protect protect) {
    std::string protect_str;
    protect_str += memory::has(protect, memory::Protect::Read) ? 'R' : '-';
    protect_str += memory::has(protect, memory::Protect::Write) ? 'W' : '-';
    protect_str += memory::has(protect, memory::Protect::Execute) ? 'X' : '-';
    if (memory::has(protect, memory::Protect::NoCache)) protect_str += "|NC";
    if (memory::has(protect, memory::Protect::WriteCombine)) protect_str += "|WC";
    return protect_str;
  };

  std::ostringstream diagnostic;
  diagnostic << "Guest memory fault: " << fault.what()
             << " [cia=0x" << std::hex << std::uppercase << state.cia
             << " nia=0x" << state.nia
             << " lr=0x" << state.lr
             << " ctr=0x" << state.ctr
             << " request=0x" << info.request_address
             << " fault=0x" << info.fault_address
             << std::dec << " width=" << info.width
             << " access=" << access_name(info.access)
             << " reason=" << reason_name(info.reason)
             << " page_state=" << page_state_name(info.page_state)
             << " mapped=" << (info.mapped ? "yes" : "no")
             << " committed=" << (info.committed ? "yes" : "no")
             << " current_protect=" << protect_string(info.current_protect)
             << " allocation_protect=" << protect_string(info.allocation_protect)
             << " page_size=0x" << std::hex << info.page_size
             << " r1=0x" << state.gpr[1]
             << " r2=0x" << state.gpr[2]
             << " r3=0x" << state.gpr[3]
             << " r4=0x" << state.gpr[4]
             << " r5=0x" << state.gpr[5]
             << " r6=0x" << state.gpr[6]
             << " r7=0x" << state.gpr[7]
             << " r8=0x" << state.gpr[8]
             << " r9=0x" << state.gpr[9]
             << " r10=0x" << state.gpr[10]
             << " r13=0x" << state.gpr[13] << ']';
  // The XEX loader already parsed the title's runtime-function directory.
  // Report coverage from that source of truth; coverage alone is not
  // evidence that a language/exception handler exists.
  const auto append_function_metadata = [&](std::string_view label,
                                            cpu::GuestAddress address) {
    diagnostic << ' ' << label << "_function=";
    if (!loaded_xex) {
      diagnostic << "no-module";
      return;
    }
    const auto& functions = loaded_xex->image.function_metadata;
    const auto it = std::find_if(functions.begin(), functions.end(),
                                 [address](const xbox::XexFunctionMetadata& fn) {
                                   return address >= fn.begin && address < fn.end;
                                 });
    if (it == functions.end()) {
      diagnostic << "none";
      return;
    }
    diagnostic << "{module="
               << (loaded_xex->image.original_pe_name.empty()
                       ? "title"
                       : loaded_xex->image.original_pe_name)
               << ",begin=0x" << std::hex << it->begin
               << ",end=0x" << it->end
               << ",unwind=0x" << it->unwind_data
               << std::dec << ",valid=" << (it->valid ? "yes" : "no") << '}';
  };
  append_function_metadata("cia", state.cia);
  append_function_metadata("lr", static_cast<cpu::GuestAddress>(state.lr));
  // Diagnostic-only guest stack walk (read-only, never changes execution):
  // standard PPC back-chain convention - [r1] = caller's saved r1, [r1+8] =
  // caller's saved LR (the return address into this frame). Best-effort:
  // a corrupt/absent frame chain stops the walk early rather than faulting
  // again while already handling a fault.
  {
    diagnostic << " stack=[";
    auto frame = static_cast<memory::GuestAddress>(state.gpr[1]);
    bool first = true;
    for (int depth = 0; depth < 16 && frame != 0u; ++depth) {
      std::uint32_t saved_lr = 0u;
      std::uint32_t next_frame = 0u;
      try {
        saved_lr = memory.read32_be(frame + 8u);
        next_frame = memory.read32_be(frame);
      } catch (const memory::MemoryFault&) {
        break;
      }
      if (!first) diagnostic << ',';
      first = false;
      diagnostic << "0x" << std::hex << saved_lr;
      append_function_metadata("stack", saved_lr);
      if (next_frame <= frame) break;  // frame chain must strictly ascend
      frame = static_cast<memory::GuestAddress>(next_frame);
    }
    diagnostic << ']' << std::dec;
  }
  const auto append_export_records = [&](std::string_view label,
                                         const std::vector<ExportTraceRecord>& records) {
    diagnostic << ' ' << label << "=[";
    bool first = true;
    for (const auto& trace : records) {
      if (!first) diagnostic << ';';
      first = false;
      diagnostic << '#' << trace.sequence << " tid=" << trace.thread_id << ' '
                 << trace.library_view() << '!';
      if (!trace.name_view().empty()) {
        diagnostic << trace.name_view();
      } else {
        diagnostic << trace.ordinal;
      }
      diagnostic << "(ord=" << trace.ordinal << ",cia=0x" << std::hex
                 << trace.call_address << ",lr=0x" << trace.lr << ",ctr=0x"
                 << trace.ctr << ",args=";
      for (const auto argument : trace.arguments) diagnostic << "0x" << argument << ',';
      diagnostic << "r3=0x" << trace.result_r3 << std::dec
                 << ",found=" << trace.handler_found
                 << ",handled=" << trace.handled
                 << ",success=" << trace.success << ')';
    }
    diagnostic << ']';
  };
  if (export_trace.enabled()) {
    append_export_records(
        "export_trace_thread",
        export_trace.recent_for_thread(thread_id, 64u));
    append_export_records("export_trace_global", export_trace.recent_global(64u));
  }
  return diagnostic.str();
}

std::string describe_dispatch_limit(const cpu::CpuState& state, const ExportTrace& export_trace,
                                    std::uint32_t thread_id) {
  std::ostringstream spin;
  spin << "Guest execution exceeded the top-level dispatch limit [cia=0x" << std::hex
       << std::uppercase << state.cia << " lr=0x" << state.lr << std::dec
       << " last_exports=[";
  const auto recent =
      export_trace.recent_for_thread(thread_id, 12u);
  for (std::size_t i = 0; i < recent.size(); ++i) {
    const auto& trace = recent[i];
    if (i != 0u) spin << ' ';
    spin << trace.library_view() << '!';
    if (!trace.name_view().empty()) {
      spin << trace.name_view();
    } else {
      spin << trace.ordinal;
    }
    spin << "(ord=" << trace.ordinal << ",lr=0x" << std::hex << trace.lr << ",r3in=0x"
         << trace.arguments[0] << ",r4=0x" << trace.arguments[1] << ",r5=0x"
         << trace.arguments[2] << ",r6=0x" << trace.arguments[3] << ",ret=0x"
         << trace.result_r3 << std::dec << ")";
  }
  spin << "]]";
  return spin.str();
}

}  // namespace xenon::core::detail
