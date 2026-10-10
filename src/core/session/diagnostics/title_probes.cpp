// Ace Combat 6 investigation probes, moved out of the generic session
// runtime with their guards, statics, guest addresses and output unchanged.
// They encode title-specific guest addresses and object layouts gathered
// while tracing the GPU/audio stall described in
// docs/runtime/AC6_RUNTIME_INVESTIGATION.md. They observe only; they must not
// grow into title fixes, and should be removed once that investigation closes.

#include <atomic>
#include <iostream>
#include <mutex>
#include <string>

#include "core/session/session_internal.hpp"
#include "xenon/logging/probe_log.hpp"

namespace xenon::core::detail {

// PM4 interrupt (first four deliveries): the handler the title dispatches to
// ([ctx+0x2A94]), its first words, and the scratch registers it acknowledges
// through.
void log_title_pm4_interrupt_handler(memory::AddressSpace& memory,
                                     gpu::GraphicsSystem& graphics_system,
                                     std::uint32_t callback_address, std::uint32_t context) {
  static std::atomic<int> logged{0};
  if (logged.fetch_add(1) >= 4) return;
  try {
    const auto handler = memory.read32_be(context + 0x2A94u);
    std::scoped_lock console_log_lock(XenonSession::console_log_mutex());
    std::cout << "[XenonSession] PM4 interrupt -> callback 0x" << std::hex
              << callback_address << " ctx=0x" << context
              << " handler=[ctx+0x2A94]=0x" << handler << " words:";
    for (std::uint32_t i = 0; i < 8u; ++i) {
      std::cout << ' ' << memory.read32_be(handler + i * 4u);
    }
    std::cout << " SCRATCH_UMSK=0x" << graphics_system.registers().read(0x1DCu)
              << " SCRATCH_ADDR=0x" << graphics_system.registers().read(0x1DDu)
              << std::dec << std::endl;
  } catch (const memory::MemoryFault&) {
  }
}

// Dispatch-trace extension for the 0x82379AC4 continuation: the object in r27
// and the global base in r29. Appends to the caller's (hex-mode) trace line.
void append_title_dispatch_probe(std::ostream& out, cpu::GuestAddress next_address,
                                 const cpu::CpuState& state, cpu::ExecutionContext& context) {
  if (next_address != 0x82379AC4u) return;
  cpu::MemoryAccessContext::PhysicalResolution probe{};
  const auto obj = static_cast<std::uint32_t>(state.gpr[27]);
  out << " r27(obj)=0x" << obj;
  if (context.memory_access.resolve_physical_ram(
          static_cast<cpu::GuestAddress>(obj) + 8u, 4, false, 4, probe)) {
    out << " obj+8=0x"
        << context.memory_access.read32_be(static_cast<cpu::GuestAddress>(obj) + 8u);
  } else {
    out << " obj+8=<unresolvable>";
  }
  const auto gbase = static_cast<std::uint32_t>(state.gpr[29]);
  out << " r29(gbase)=0x" << gbase;
  if (context.memory_access.resolve_physical_ram(
          static_cast<cpu::GuestAddress>(gbase) + 0x3B38u, 4, false, 4, probe)) {
    out << " gbase+3B38=0x"
        << context.memory_access.read32_be(static_cast<cpu::GuestAddress>(gbase) + 0x3B38u);
  } else {
    out << " gbase+3B38=<unresolvable>";
  }
}

// ExCreateThread for the 0x821EEDE0 render-setup thread entry.
void log_title_thread_creation(cpu::GuestAddress start_address, std::uint64_t caller_lr,
                               std::uint64_t start_context, std::uint32_t creation_flags) {
  if (start_address != 0x821EEDE0u) return;
  logging::append_probe_log("ex_create_thread_caller_diag.log",
                            "ExCreateThread(821EEDE0): caller_lr=0x%08llX start_context=0x%08llX "
                            "creation_flags=0x%08X\n",
                            (unsigned long long)caller_lr, (unsigned long long)start_context,
                            creation_flags);
}

// Per-vsync samples of the event-dispatcher state, the subsystem label, the
// two render-setup handshake events and the vsync ISR's counter chain.
void sample_title_vsync_probes(memory::AddressSpace& memory, std::uint32_t interrupt_context) {
  // Event-dispatcher probe (0x823AD848 family): a global pointer at
  // 0x82916E4C leads to a per-subsystem struct whose +0x12C field looked,
  // from static reading, like a pending-event bitmask and +0x130 like a
  // tick counter guarding which handler branch runs. Sampled here (host
  // side, every vsync) rather than inside the generated shard itself,
  // since that shard's custom incremental-build cache was not picking up
  // edits reliably. Logged on change only, uncapped, to catch a rare
  // transition without flooding when the value is static.
  {
    static std::atomic<std::uint32_t> _last_bitmask{0xFFFFFFFFu};
    static std::atomic<std::uint32_t> _last_counter{0xFFFFFFFFu};
    try {
      const auto ctx_ptr = memory.read32_be(static_cast<cpu::GuestAddress>(0x82916E4Cu));
      if (ctx_ptr != 0u) {
        const auto bitmask = memory.read32_be(static_cast<cpu::GuestAddress>(ctx_ptr + 0x12Cu));
        const auto counter = memory.read32_be(static_cast<cpu::GuestAddress>(ctx_ptr + 0x130u));
        if (bitmask != _last_bitmask.load() || counter != _last_counter.load()) {
          _last_bitmask.store(bitmask);
          _last_counter.store(counter);
          logging::append_probe_log("event_dispatch_823AD848_diag.log", "823AD848 probe CHANGED: ctx_ptr=0x%08X bitmask=0x%08X counter=0x%08X\n",
                       ctx_ptr, bitmask, counter);
        }
      }
    } catch (const std::exception&) {
      // Guest memory not mapped yet (early boot) - ignore, next tick retries.
    }
  }

  // One-time read of the guest C-string at 0x82067EC8, the label argument
  // the subsystem-registration call (xenon_fn_821E4AD0 -> 0x821DD028)
  // passes alongside the object that owns the two stuck handshake events -
  // to identify semantically what this subsystem actually is.
  {
    static std::atomic<bool> _label_dumped{false};
    if (!_label_dumped.load()) {
      try {
        std::string label;
        for (std::uint32_t i = 0; i < 64u; ++i) {
          const auto c = memory.read8(static_cast<cpu::GuestAddress>(0x82067EC8u + i));
          if (c == 0) break;
          label.push_back(static_cast<char>(c));
        }
        if (!label.empty()) {
          _label_dumped.store(true);
          logging::append_probe_log("subsystem_label_diag.log", "label at 0x82067EC8: \"%s\"\n",
                                    label.c_str());
        }
      } catch (const std::exception&) {
      }
    }
  }

  // Raw guest-memory probe on the two xenon_fn_821EEDE0 render-setup
  // threads' own per-thread handshake events (X_DISPATCH_HEADER SignalState
  // field, header+0x4) at 0x62D74 and 0x62DC4. Both threads are confirmed
  // (live debugger) permanently blocked in KeWaitForSingleObject on these
  // exact headers, and KeSetEvent is confirmed (full-run log) to never be
  // called for either. This probe tests whether the GUEST's own memory at
  // the SignalState offset ever flips to nonzero anyway (i.e. the game
  // signals via a direct memory write, bypassing the KeSetEvent export
  // entirely - a legitimate real-hardware "fast path" pattern) while
  // Xenon's host-side KernelEvent, which only reads this field once at
  // first resolution, never finds out. Logged on change only.
  {
    static std::atomic<std::uint32_t> _last_sig1{0xFFFFFFFFu};
    static std::atomic<std::uint32_t> _last_sig2{0xFFFFFFFFu};
    try {
      const auto sig1 = memory.read32_be(static_cast<cpu::GuestAddress>(0x00062D78u));
      const auto sig2 = memory.read32_be(static_cast<cpu::GuestAddress>(0x00062DC8u));
      if (sig1 != _last_sig1.load() || sig2 != _last_sig2.load()) {
        _last_sig1.store(sig1);
        _last_sig2.store(sig2);
        logging::append_probe_log("handshake_event_raw_memory_diag.log", "raw SignalState CHANGED: addr62D74+4(0x62D78)=0x%08X addr62DC4+4(0x62DC8)=0x%08X\n",
                     sig1, sig2);
      }
    } catch (const std::exception&) {
      // Guest memory not mapped yet (early boot) - ignore, next tick retries.
    }
  }

  // Probe the vsync ISR's own per-tick dispatch-target chain: the guest ISR
  // (0x821E63F0, via its real per-tick callee 0x821EFBE0) resolves
  // obj = read32(ctx + 0x2A94) and then conditionally writes into
  // obj + 0x4 (the X_DISPATCH_HEADER SignalState offset) once an internal
  // counter wraps. ctx here is the exact same guest address as
  // interrupt_context. Logged on change only.
  {
    static std::atomic<std::uint32_t> _last_cur{0xFFFFFFFFu};
    static std::atomic<std::uint32_t> _last_target{0xFFFFFFFFu};
    static std::atomic<std::uint32_t> _last_tickcount{0xFFFFFFFFu};
    static std::atomic<std::uint32_t> _last_gate1{0xFFFFFFFFu};
    static std::atomic<std::uint32_t> _last_gate2{0xFFFFFFFFu};
    try {
      if (interrupt_context != 0) {
        // ctx+0x412C = "current processed" counter, ctx+0x4130 = "target/pending"
        // counter inside the guest ISR's real per-tick handler (0x821EFBE0). When
        // equal, the ISR skips its entire processing chain (including the write at
        // obj+0x4) and does nothing but bump ctx+0x4094 (a plain call counter).
        // ctx+0x4130 is only ever incremented by the producer at xenon_fn_821EFCE0,
        // which itself early-outs unless ctx+0x409C != ctx+0x4094 (gate1) and
        // ctx+0x40A4 != 1 (gate2).
        const auto cur = memory.read32_be(static_cast<cpu::GuestAddress>(interrupt_context + 0x412Cu));
        const auto target = memory.read32_be(static_cast<cpu::GuestAddress>(interrupt_context + 0x4130u));
        const auto tickcount = memory.read32_be(static_cast<cpu::GuestAddress>(interrupt_context + 0x4094u));
        const auto gate1 = memory.read32_be(static_cast<cpu::GuestAddress>(interrupt_context + 0x409Cu));
        const auto gate2 = memory.read32_be(static_cast<cpu::GuestAddress>(interrupt_context + 0x40A4u));
        if (cur != _last_cur.load() || target != _last_target.load() || tickcount != _last_tickcount.load() ||
            gate1 != _last_gate1.load() || gate2 != _last_gate2.load()) {
          _last_cur.store(cur);
          _last_target.store(target);
          _last_tickcount.store(tickcount);
          _last_gate1.store(gate1);
          _last_gate2.store(gate2);
          logging::append_probe_log("vsync_isr_obj_chain_diag.log",
                       "vsync ISR counters CHANGED: ctx=0x%08X cur[+0x412C]=0x%08X target[+0x4130]=0x%08X tickcount[+0x4094]=0x%08X gate1[+0x409C]=0x%08X gate2[+0x40A4]=0x%08X\n",
                       (unsigned)interrupt_context, cur, target, tickcount, gate1, gate2);
        }
      }
    } catch (const std::exception&) {
      // Guest memory not mapped yet (early boot) - ignore, next tick retries.
    }
  }
}

}  // namespace xenon::core::detail
