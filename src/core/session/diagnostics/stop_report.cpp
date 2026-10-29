#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "xenon/core/guest_stack_walk.hpp"
#include "xenon/core/session.hpp"
#include "xenon/kernel/handle_table.hpp"
#include "xenon/kernel/object_description.hpp"
#include "xenon/logging/diagnostic_events.hpp"
#include "xenon/memory/address_space.hpp"
#if defined(XENON_HAS_AUDIO)
#include "xenon/audio/system.hpp"
#endif

namespace xenon::core {

std::string XenonSession::describe_present_results() {
  static constexpr std::array<const char*, 8> kNames{
      "ok", "suboptimal", "out of date", "surface lost", "minimized", "not configured",
      "unsupported", "error"};
  std::string text;
  for (std::size_t i = 0; i < gpu_present_results_.size(); ++i) {
    const auto count = gpu_present_results_[i].load(std::memory_order_relaxed);
    if (!count) continue;
    text += (text.empty() ? " (" : ", ") + std::to_string(count) + " " + kNames[i];
  }
  if (text.empty()) return text;
  std::scoped_lock lock(gpu_present_error_mutex_);
  return text + (gpu_present_error_.empty() ? ")" : "; last failure: " + gpu_present_error_ + ")");
}

std::vector<std::string> XenonSession::describe_pump_state() {
  std::vector<std::string> lines;
  // A pump that stopped because a guest callback never returned.
  const auto now = CallbackWatch::now_ns();
  lines.push_back(CallbackWatch::describe("GPU pump", gpu_callback_watch_.snapshot(), now));
  lines.push_back(
      CallbackWatch::describe("GPU vsync", gpu_vsync_callback_watch_.snapshot(), now));
  // Why the GPU worker woke: published ring work, a vsync's present request,
  // or a timed poll (parked on WAIT_REG_MEM, or the idle fallback).
  lines.push_back("GPU worker wakes: ring work " + std::to_string(gpu_wake_.count(kGpuWakeRingWork)) +
                  ", present " + std::to_string(gpu_wake_.count(kGpuWakePresent)) + ", polls " +
                  std::to_string(gpu_wake_.timeouts()) + "; frames presented " +
                  std::to_string(gpu_presents_.load()) + describe_present_results());
  lines.push_back(CallbackWatch::describe("Audio pump", audio_callback_watch_.snapshot(), now));
  const auto interrupt = kernel_process_ ? kernel_process_->gpu_interrupt_callback()
                                         : kernel::KernelProcess::GpuInterruptCallbackState{};
  std::ostringstream vsync;
  vsync << "GPU vsync interrupt: callback=0x" << std::hex << interrupt.callback_address
        << " context=0x" << interrupt.context << std::dec
        << " delivered=" << gpu_interrupts_delivered_.load()
        << " failed=" << gpu_interrupts_failed_.load();
  lines.push_back(vsync.str());
  if (memory_) {
    // Who owns the 512 MiB: a title that exhausts physical memory sees
    // MmAllocatePhysicalMemoryEx/NtAllocateVirtualMemory fail and usually
    // dereferences the null result shortly after.
    const auto stats = memory_->memory_statistics();
    const auto mib = [](std::uint32_t pages) {
      return std::to_string(pages * (memory::kBasePageSize / 1024u) / 1024u) + " MiB";
    };
    lines.push_back("Guest physical memory: " + mib(stats.available_physical_pages) +
                    " available of " + mib(stats.total_physical_pages) + "; system " +
                    mib(stats.system_physical_pages) + ", virtual commits " +
                    mib(stats.anonymous_physical_pages) + ", physical allocations " +
                    mib(stats.explicit_physical_pages) + ", retired " +
                    mib(stats.retired_physical_pages) + "; image " + mib(stats.image_pages) +
                    ", virtual reserved " + mib(stats.reserved_virtual_pages) + " committed " +
                    mib(stats.committed_virtual_pages));
  }
#if defined(XENON_HAS_AUDIO)
  if (audio_) {
    // Why each render client's callbacks stopped, if they did: a callback
    // needs a credit, and a credit returns only when a submitted frame has
    // been consumed.
    const auto clients = audio_->render_client_stats();
    lines.push_back("Audio render clients: " + std::to_string(clients.size()) +
                    "; submissions to unregistered handles: " +
                    std::to_string(audio_->rejected_unregistered_submissions()));
    for (const auto& client : clients) lines.push_back("  " + audio::AudioSystem::describe(client));
    // Whether anything audible reached the device, and the recording, if one.
    lines.push_back(audio::OutputMeter::describe(audio_->output_levels(), audio::kXboxSampleRate) +
                    "; underruns " + std::to_string(audio_->underrun_count()) +
                    "; loudest submitted sample " + std::to_string(audio_->submitted_peak()));
    lines.push_back(audio::XmaDecoder::describe(audio_->xma().stats()));
    if (const auto& recorder = audio_->output_recorder(); recorder.recording())
      lines.push_back("Audio capture: " + std::to_string(recorder.frames_written()) +
                      " frames written, " + std::to_string(recorder.frames_dropped()) + " dropped");
  }
#endif
  return lines;
}

void XenonSession::report_pump_state(std::string_view reason) {
  if (!config_.enable_logging) return;
  const auto lines = describe_pump_state();
  std::scoped_lock console_log_lock(console_log_mutex());
  std::cout << "[XenonSession] Pump state (" << reason << "):" << std::endl;
  for (const auto& line : lines) std::cout << "[XenonSession]   " << line << std::endl;
}

// Stop-time evidence for a guest that is still executing: what each thread
// last asked the kernel for, GPU ring/fence state, every guest thread's
// registers and the memory-watch history. Diagnostic-only; reads are racy by
// design.
void XenonSession::report_stop_diagnostics() {
  report_pump_state("stop requested");
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
    for (const auto& callback : logging::events::open_callbacks(snapshot)) {
      std::cout << "[XenonSession]   t" << callback.guest_thread_id << " is inside the "
                << (callback.source ? callback.source : "guest") << " callback 0x" << std::hex
                << callback.guest_address << std::dec << " since "
                << (now_ns - callback.host_time_ns) / 1'000'000 << " ms ago" << std::endl;
    }
    if (const auto parked = logging::events::parked_gpu_wait(snapshot)) {
      std::cout << "[XenonSession]   GPU parked on WAIT_REG_MEM 0x" << std::hex
                << parked->guest_address << " for reference 0x" << parked->value << std::dec
                << " since " << (now_ns - parked->host_time_ns) / 1'000'000 << " ms ago"
                << std::endl;
    }
  }
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
        std::cout << "[XenonSession]   t" << thread_id;
        if (const auto* kernel_thread =
                kernel_process_ ? kernel_process_->thread_manager().get_thread_ptr(thread_id)
                                : nullptr) {
          if (const auto name = kernel_thread->name(); !name.empty())
            std::cout << " (" << name << ')';
        }
        std::cout << " in ";
        if (call.descriptor) {
          std::cout << call.descriptor->library << '!' << call.descriptor->name;
        } else {
          std::cout << "ordinal " << call.ordinal;
        }
        std::cout << " for " << waited_ms << "ms lr=0x" << std::hex << call.lr << " r3=0x"
                  << call.arguments[0] << " r4=0x" << call.arguments[1] << " r5=0x"
                  << call.arguments[2] << " r6=0x" << call.arguments[3] << std::dec
                  << std::endl;
        // A Ke wait names guest dispatcher objects directly; show each one's
        // DISPATCHER_HEADER (Type at +0, SignalState at +4) so a wait on an
        // object nothing signals is visible.
        if (call.descriptor && memory_ &&
            (call.descriptor->name == "KeWaitForSingleObject" ||
             call.descriptor->name == "KeWaitForMultipleObjects")) {
          const bool multiple = call.descriptor->name == "KeWaitForMultipleObjects";
          const auto count = multiple ? std::min<std::uint64_t>(call.arguments[0], 16u) : 1u;
          std::cout << "[XenonSession]     objects:" << std::hex;
          for (std::uint64_t i = 0; i < count; ++i) {
            try {
              const auto object = multiple
                  ? memory_->read32_be(static_cast<cpu::GuestAddress>(call.arguments[1] + i * 4u))
                  : static_cast<std::uint32_t>(call.arguments[0]);
              std::cout << " 0x" << object << "{type=" << unsigned(memory_->read8(object))
                        << " signal=0x" << memory_->read32_be(object + 4u) << '}';
            } catch (const memory::MemoryFault&) {
              std::cout << " <unreadable>";
            }
          }
          std::cout << std::dec << std::endl;
        }
        // An Nt wait names handles; resolve each to its object's state and
        // the guest signal last noted on it, so the report says who should
        // have released the waiter and when it last did.
        if (call.descriptor && kernel_process_ &&
            (call.descriptor->name == "NtWaitForSingleObjectEx" ||
             call.descriptor->name == "NtSignalAndWaitForSingleObjectEx" ||
             call.descriptor->name == "NtWaitForMultipleObjectsEx")) {
          const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
          const auto describe = [&](const char* role, std::uint32_t handle) {
            kernel::HandleView view{};
            std::cout << "[XenonSession]     " << role << " handle 0x" << std::hex << handle
                      << std::dec << ": ";
            if (kernel_process_->handle_table().lookup(handle, view) !=
                    kernel::KernelIoCode::Success ||
                !view.object) {
              std::cout << "<not a live handle>" << std::endl;
              return;
            }
            std::cout << kernel::describe_object_state(*view.object, now_ns) << std::endl;
          };
          if (call.descriptor->name == "NtWaitForMultipleObjectsEx") {
            // r3 = count, r4 = handle array.
            const auto count = std::min<std::uint64_t>(call.arguments[0], 16u);
            for (std::uint64_t i = 0; memory_ && i < count; ++i) {
              try {
                describe("wait", memory_->read32_be(
                                     static_cast<cpu::GuestAddress>(call.arguments[1] + i * 4u)));
              } catch (const memory::MemoryFault&) {
                std::cout << "[XenonSession]     wait handle array unreadable" << std::endl;
                break;
              }
            }
          } else if (call.descriptor->name == "NtSignalAndWaitForSingleObjectEx") {
            describe("signalled", static_cast<std::uint32_t>(call.arguments[0]));
            describe("wait", static_cast<std::uint32_t>(call.arguments[1]));
          } else {
            describe("wait", static_cast<std::uint32_t>(call.arguments[0]));
          }
        }
        // A critical section names its owner: X_RTL_CRITICAL_SECTION has
        // LockCount at +0x10, RecursionCount at +0x14 and the owning
        // thread's KTHREAD at +0x18.
        if (call.descriptor && memory_ && call.descriptor->name == "RtlEnterCriticalSection") {
          const auto cs = static_cast<cpu::GuestAddress>(call.arguments[0]);
          try {
            const auto lock_count = static_cast<std::int32_t>(memory_->read32_be(cs + 0x10u));
            const auto recursion = memory_->read32_be(cs + 0x14u);
            const auto owner = memory_->read32_be(cs + 0x18u);
            std::cout << "[XenonSession]     critical section 0x" << std::hex << cs
                      << ": lock_count=" << std::dec << lock_count << " recursion=" << recursion
                      << " owner_kthread=0x" << std::hex << owner << std::dec;
            if (kernel_process_) {
              for (const auto& candidate : kernel_process_->thread_manager().enumerate_threads()) {
                if (owner != 0u && candidate->guest_kthread_address() == owner) {
                  std::cout << " (t" << candidate->thread_id() << ' ' << candidate->name()
                            << (candidate->is_terminated() ? ", terminated" : "") << ')';
                }
              }
            }
            std::cout << std::endl;
          } catch (const memory::MemoryFault&) {
          }
        }
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
      if (graphics_system_ && kernel_process_) {
        // Racy diagnostic read of counters the pump thread updates.
        const auto& gpu_stats = graphics_system_->command_processor().statistics();
        const auto ring = kernel_process_->gpu_ring_buffer();
        const auto front = kernel_process_->gpu_front_buffer();
        std::cout << "[XenonSession] GPU: packets=" << gpu_stats.packets
                  << " draws=" << gpu_stats.draws << " events=" << gpu_stats.event_packets
                  << " interrupts=" << gpu_stats.interrupt_packets
                  << " wait_stalls=" << gpu_stats.wait_stalls
                  << " unexecuted=" << gpu_stats.unexecuted_packets
                  << " drains=" << gpu_stats.ring_drains
                  << " incomplete=" << gpu_stats.incomplete_ring_packets
                  << " parks=" << gpu_stats.parks_resumed << " parked_ms="
                  << gpu_stats.parked_ns / 1'000'000 << " longest_park_ms="
                  << gpu_stats.longest_park_ns / 1'000'000
                  << " guest_swaps=" << kernel_process_->gpu_front_buffer().generation
                  << " ring(read=" << ring.read_index << " write=" << ring.write_index
                  << " cap=" << ring.capacity_dwords << ")"
                  << " front_buffer=0x" << std::hex << front.base_address << std::dec << " "
                  << front.width << "x" << front.height << std::endl;
        if (gpu_) {
          const auto performance = gpu_->performance_counters();
          std::cout << "[XenonSession] GPU backend: draws " << performance.draws << " ("
                    << performance.failed_draws << " failed), resolves "
                    << performance.resolve_operations << " (" << performance.failed_resolves
                    << " failed); last error: "
                    << (gpu_->last_error().empty() ? "none" : gpu_->last_error()) << std::endl;
        }
        // Whether the scanout source holds an image at all: a black window
        // with an all-zero front buffer is a rendering problem, not a
        // presentation one.
        if (front.base_address && front.width && front.height && memory_) {
          std::vector<std::byte> pixels(std::size_t(front.width) * front.height * 4u);
          if (memory_->copy_physical_range(front.base_address & 0x1FFFFFFFu, pixels)) {
            std::size_t nonzero = 0;
            std::set<std::uint32_t> distinct;
            for (std::size_t i = 0; i + 4u <= pixels.size(); i += 4u) {
              std::uint32_t value{};
              std::memcpy(&value, pixels.data() + i, 4u);
              if (value & 0x00FFFFFFu && value & 0xFFFFFF00u) ++nonzero;
              if (distinct.size() < 64u) distinct.insert(value);
            }
            std::cout << "[XenonSession] Front buffer content: " << nonzero * 100u / (pixels.size() / 4u)
                      << "% of pixels non-black, " << distinct.size()
                      << (distinct.size() >= 64u ? "+" : "") << " distinct values" << std::endl;
          }
        }
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
        std::cout << "[XenonSession]   t" << thread_id;
        if (const auto* kernel_thread =
                kernel_process_ ? kernel_process_->thread_manager().get_thread_ptr(thread_id)
                                : nullptr) {
          if (const auto name = kernel_thread->name(); !name.empty())
            std::cout << " (" << name << ')';
        }
        std::cout << " cia=0x" << std::hex
                  << guest_state->cia << " nia=0x" << guest_state->nia << " lr=0x"
                  << guest_state->lr << " ctr=0x" << guest_state->ctr << " r1=0x"
                  << guest_state->gpr[1] << " r3=0x" << guest_state->gpr[3] << " r4=0x"
                  << guest_state->gpr[4] << std::dec << std::endl;
        // Its callers, which say what a thread spinning in guest code is
        // waiting for (see walk_guest_stack() for the frame convention).
        if (memory_) {
          std::cout << "[XenonSession]     stack:" << std::hex;
          for (const auto& frame :
               walk_guest_stack(*memory_, static_cast<std::uint32_t>(guest_state->gpr[1])))
            std::cout << " 0x" << frame.return_address;
          std::cout << std::dec << std::endl;
        }
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
