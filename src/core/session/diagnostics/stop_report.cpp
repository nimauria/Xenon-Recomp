#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "xenon/core/session.hpp"
#include "xenon/logging/diagnostic_events.hpp"
#if defined(XENON_HAS_AUDIO)
#include "xenon/audio/system.hpp"
#endif

namespace xenon::core {

// Stop-time evidence for a guest that is still executing: what each thread
// last asked the kernel for, GPU ring/fence state, every guest thread's
// registers and the memory-watch history. Diagnostic-only; reads are racy by
// design.
void XenonSession::report_stop_diagnostics() {
  // A stop that finds the guest still executing usually means it is stalled
  // (blocked in a kernel wait or spinning). Say what each thread last asked the
  // kernel for, so the stall can be diagnosed from the log alone.
  if (logging::events::enabled()) {
    // Recorded wait and signal events (XENON_DIAG_EVENTS=1): which guest
    // thread still waits on which object, and which thread last signalled it.
    const auto snapshot = logging::events::snapshot();
    const auto waits = logging::events::blocked_waits(snapshot);
    const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    std::istringstream lines(logging::events::format_blocked_waits(waits, now_ns));
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] Diagnostic events: " << snapshot.size() << " retained of "
              << logging::events::recorded() << " recorded; open waits: " << waits.size()
              << std::endl;
    for (std::string line; std::getline(lines, line);)
      std::cout << "[XenonSession]   " << line << std::endl;
  }
#if defined(XENON_HAS_AUDIO)
  if (config_.enable_logging && audio_) {
    // Why each render client's callbacks stopped, if they did: a callback
    // needs a credit, and a credit returns only when a submitted frame has
    // been consumed.
    const auto clients = audio_->render_client_stats();
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] Audio render clients: " << clients.size()
              << "; submissions to unregistered handles: "
              << audio_->rejected_unregistered_submissions() << std::endl;
    for (const auto& client : clients)
      std::cout << "[XenonSession]   " << audio::AudioSystem::describe(client) << std::endl;
  }
#endif
  if (config_.enable_logging && export_trace_.enabled()) {
    const auto recent = export_trace_.recent_global(16u);
    std::scoped_lock console_log_lock(console_log_mutex());
    {
      std::scoped_lock lock(in_flight_exports_mutex_);
      const auto now = std::chrono::steady_clock::now();
      std::cout << "[XenonSession] Threads currently blocked inside a kernel call: "
                << in_flight_exports_.size() << std::endl;
      for (const auto& [thread_id, call] : in_flight_exports_) {
        const auto waited_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - call.since).count();
        std::cout << "[XenonSession]   t" << thread_id << " in ";
        if (call.descriptor) {
          std::cout << call.descriptor->library << '!' << call.descriptor->name;
        } else {
          std::cout << "ordinal " << call.ordinal;
        }
        std::cout << " for " << waited_ms << "ms lr=0x" << std::hex << call.lr << " r3=0x"
                  << call.arguments[0] << " r4=0x" << call.arguments[1] << " r5=0x"
                  << call.arguments[2] << " r6=0x" << call.arguments[3] << std::dec
                  << std::endl;
        // A thread stuck acquiring a spin lock: the lock word holds the owner's id.
        if (call.descriptor && call.descriptor->name.find("SpinLock") != std::string::npos &&
            memory_) {
          try {
            std::cout << "[XenonSession]     lock word at 0x" << std::hex << call.arguments[0]
                      << " = 0x"
                      << memory_->read32_be(static_cast<cpu::GuestAddress>(call.arguments[0]))
                      << " (owner thread id)" << std::dec << std::endl;
          } catch (const memory::MemoryFault&) {
          }
          // How did this thread get here? Its own recent kernel calls show whether
          // it already took (and failed to release) the lock.
          for (const auto& trace : export_trace_.recent_for_thread(thread_id, 12u)) {
            std::cout << "[XenonSession]     earlier: " << trace.library_view() << '!'
                      << (trace.name_view().empty() ? std::string("?")
                                                    : std::string(trace.name_view()))
                      << " lr=0x" << std::hex << trace.lr << " r3=0x" << trace.arguments[0]
                      << " r4=0x" << trace.arguments[1] << " -> 0x" << trace.result_r3
                      << std::dec << std::endl;
          }
        }
      }
      const auto interrupt = kernel_process_ ? kernel_process_->gpu_interrupt_callback()
                                             : kernel::KernelProcess::GpuInterruptCallbackState{};
      std::cout << "[XenonSession] GPU vsync interrupt: callback=0x" << std::hex
                << interrupt.callback_address << " context=0x" << interrupt.context << std::dec
                << " delivered=" << gpu_interrupts_delivered_.load()
                << " failed=" << gpu_interrupts_failed_.load() << std::endl;
      if (graphics_system_ && kernel_process_) {
        // Racy diagnostic read of counters the pump thread updates.
        const auto& gpu_stats = graphics_system_->command_processor().statistics();
        const auto ring = kernel_process_->gpu_ring_buffer();
        const auto front = kernel_process_->gpu_front_buffer();
        std::cout << "[XenonSession] GPU: packets=" << gpu_stats.packets
                  << " draws=" << gpu_stats.draws << " events=" << gpu_stats.event_packets
                  << " interrupts=" << gpu_stats.interrupt_packets
                  << " wait_stalls=" << gpu_stats.wait_stalls
                  << " ring(read=" << ring.read_index << " write=" << ring.write_index
                  << " cap=" << ring.capacity_dwords << ")"
                  << " front_buffer=0x" << std::hex << front.base_address << std::dec << " "
                  << front.width << "x" << front.height << std::endl;
        const auto& stall = graphics_system_->command_processor().last_wait_stall();
        if (stall.valid) {
          std::cout << "[XenonSession] GPU parked on WAIT_REG_MEM: "
                    << (stall.memory ? "memory 0x" : "register 0x") << std::hex << stall.address
                    << " wait_info=0x" << stall.wait_info << " reference=0x" << stall.reference
                    << " mask=0x" << stall.mask << " last value=0x" << stall.last_value
                    << std::dec << std::endl;
        }
        // The ring words at the stalled position show what the GPU is waiting behind.
        if (ring.configured() && memory_) {
          std::cout << "[XenonSession] GPU ring @read:";
          for (std::uint32_t i = 0; i < 24u; ++i) {
            std::array<std::byte, 4> word{};
            const auto index = (ring.read_index + i) % ring.capacity_dwords;
            std::uint32_t value = 0;
            if (memory_->copy_physical_range(ring.base_address + index * 4u, word)) {
              value = (std::uint32_t(word[0]) << 24) | (std::uint32_t(word[1]) << 16) |
                      (std::uint32_t(word[2]) << 8) | std::uint32_t(word[3]);
            }
            std::cout << ' ' << std::hex << value << std::dec;
          }
          std::cout << std::endl;
          // If the stalled ring packet is an INDIRECT_BUFFER, show the words around
          // the WAIT_REG_MEM inside it (the fence the GPU is parked on).
          std::array<std::byte, 12> head{};
          if (memory_->copy_physical_range(ring.base_address + ring.read_index * 4u, head)) {
            const auto be = [&](std::size_t i) {
              return (std::uint32_t(head[i]) << 24) | (std::uint32_t(head[i + 1]) << 16) |
                     (std::uint32_t(head[i + 2]) << 8) | std::uint32_t(head[i + 3]);
            };
            if ((be(0) & 0xFFFF0000u) == 0xC0010000u && ((be(0) >> 8) & 0xFFu) == 0x3Fu) {
              const std::uint32_t ib_address = be(4) & 0x1FFFFFFFu;
              const std::uint32_t ib_length = std::min<std::uint32_t>(be(8) & 0xFFFFFu, 8192u);
              std::vector<std::uint32_t> ib(ib_length);
              std::vector<std::byte> raw(ib_length * 4u);
              if (memory_->copy_physical_range(ib_address, raw)) {
                for (std::uint32_t i = 0; i < ib_length; ++i) {
                  ib[i] = (std::uint32_t(raw[i * 4]) << 24) | (std::uint32_t(raw[i * 4 + 1]) << 16) |
                          (std::uint32_t(raw[i * 4 + 2]) << 8) | std::uint32_t(raw[i * 4 + 3]);
                }
                for (std::uint32_t i = 0; i < ib_length; ++i) {
                  if ((ib[i] & 0xFFFFFFFEu) == 0xC0043C00u && i + 3u < ib_length && ib[i + 2u] == stall.address && ib[i + 3u] == stall.reference) {
                    std::cout << "[XenonSession] GPU IB 0x" << std::hex << ib_address
                              << " len=" << std::dec << ib_length << " wait at dword " << i
                              << ", context:" << std::hex;
                    for (std::uint32_t j = (i > 40u ? i - 40u : 0u);
                         j < std::min<std::uint32_t>(ib_length, i + 24u); ++j) {
                      std::cout << (j == i ? " [" : " ") << ib[j];
                    }
                    std::cout << std::dec << std::endl;
                    break;
                  }
                }
              }
            }
          }
        }
      }
      std::cout << "[XenonSession] Guest threads running outside a kernel call (cia is the last "
                   "recorded control-flow point):" << std::endl;
      for (const auto& [thread_id, guest_state] : guest_thread_states_) {
        if (in_flight_exports_.contains(thread_id)) continue;
        std::cout << "[XenonSession]   t" << thread_id << " cia=0x" << std::hex
                  << guest_state->cia << " nia=0x" << guest_state->nia << " lr=0x"
                  << guest_state->lr << " ctr=0x" << guest_state->ctr << " r1=0x"
                  << guest_state->gpr[1] << " r3=0x" << guest_state->gpr[3] << " r4=0x"
                  << guest_state->gpr[4] << std::dec << std::endl;
        if (config_.verbose_logging) {
          // A stopped thread that is spinning entirely in guest code often has
          // no recent export to identify what it is waiting for. Preserve the
          // complete architectural register set and bounded snapshots of the
          // conventional nonvolatile object-pointer registers. This is
          // diagnostic-only, read-only, and title agnostic; it deliberately
          // runs only for verbose stop reports so normal logs stay compact.
          for (std::size_t base = 0; base < guest_state->gpr.size(); base += 4u) {
            std::cout << "[XenonSession]     gpr:";
            for (std::size_t index = base;
                 index < (std::min)(base + 4u, guest_state->gpr.size()); ++index) {
              std::cout << " r" << std::dec << index << "=0x" << std::hex
                        << guest_state->gpr[index];
            }
            std::cout << std::dec << std::endl;
          }

          if (memory_) {
            std::set<cpu::GuestAddress> dumped;
            for (const std::size_t index : {31u, 30u, 29u}) {
              const auto address = static_cast<cpu::GuestAddress>(guest_state->gpr[index]);
              const auto start = address & ~cpu::GuestAddress{0xFu};
              if (address == 0u || !dumped.insert(start).second) continue;
              const auto mapping = memory_->query(address);
              if (!mapping || mapping->state != memory::PageState::Committed ||
                  !memory::has(mapping->current_protect, memory::Protect::Read)) {
                continue;
              }
              std::cout << "[XenonSession]     r" << index << " pointee=0x" << std::hex
                        << address << " allocation=0x" << mapping->allocation_base << "+0x"
                        << mapping->allocation_size << " snapshot:" << std::dec << std::endl;
              for (std::uint32_t offset = 0u; offset < 0x600u; offset += 16u) {
                std::cout << "[XenonSession]       0x" << std::hex << (start + offset) << ':';
                bool readable = true;
                try {
                  for (std::uint32_t byte = 0u; byte < 16u; ++byte) {
                    std::cout << ' ' << std::setw(2) << std::setfill('0')
                              << static_cast<unsigned>(memory_->read8(start + offset + byte));
                  }
                } catch (const memory::MemoryFault&) {
                  readable = false;
                }
                std::cout << std::setfill(' ') << std::dec;
                if (!readable) std::cout << " <unreadable>";
                std::cout << std::endl;
                if (!readable) break;
              }
            }
          }
        }
        // The ring is shared and busy, so a spinning thread's own history can be
        // long gone; whatever is still in it is the best evidence of how it got here.
        for (const auto& trace : export_trace_.recent_for_thread(thread_id, 60u)) {
          std::cout << "[XenonSession]     earlier: " << trace.library_view() << '!'
                    << (trace.name_view().empty() ? std::string("?") : std::string(trace.name_view()))
                    << " ord=" << trace.ordinal << " lr=0x" << std::hex << trace.lr << " r3=0x"
                    << trace.arguments[0] << " r4=0x" << trace.arguments[1] << " -> 0x"
                    << trace.result_r3 << std::dec << std::endl;
        }
      }
    }
    std::cout << "[XenonSession] Stop requested while the guest was running; last "
              << recent.size() << " kernel calls (oldest first):" << std::endl;
    for (const auto& trace : recent) {
      std::cout << "[XenonSession]   t" << trace.thread_id << ' ' << trace.library_view() << '!';
      if (!trace.name_view().empty()) {
        std::cout << trace.name_view();
      } else {
        std::cout << trace.ordinal;
      }
      std::cout << " lr=0x" << std::hex << trace.lr << " r3=0x" << trace.arguments[0]
                << " r4=0x" << trace.arguments[1] << " r5=0x" << trace.arguments[2]
                << " r6=0x" << trace.arguments[3] << " -> 0x" << trace.result_r3 << std::dec
                << std::endl;
    }
  }

  if (config_.enable_logging && memory_watch_.active()) {
    // One final boundary sample so a change that landed after the last poll shows
    // up, then the whole retained history (oldest first) and the current words.
    WatchObserver final_sample{};
    final_sample.phase = WatchPhase::Poll;
    final_sample.note = "stop-report";
    sample_memory_watch(final_sample);
    const auto history = memory_watch_.history();
    std::scoped_lock console_log_lock(console_log_mutex());
    std::cout << "[XenonSession] Memory watch: " << memory_watch_.addresses().size()
              << " word(s), " << memory_watch_.total_changes() << " change(s), "
              << history.size() << " retained record(s):" << std::endl;
    for (const auto& record : history) {
      std::cout << "[XenonSession]   " << GuestMemoryWatch::format(record) << std::endl;
    }
    for (const auto address : memory_watch_.addresses()) {
      const auto value = memory_watch_.last_value(address);
      std::cout << "[XenonSession]   0x" << std::hex << std::uppercase << address << " = ";
      if (value) {
        std::cout << "0x" << *value;
      } else {
        std::cout << "<unreadable>";
      }
      std::cout << std::dec << std::endl;
    }
  }

}

}  // namespace xenon::core
