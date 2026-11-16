#pragma once

#include <cstdint>
#include <deque>
#include <span>
#include <functional>
#include <utility>
#include <vector>

#include "xenon/gpu/backend.hpp"
#include "xenon/gpu/backend_executor.hpp"
#include "xenon/gpu/perf_stats.hpp"
#include "xenon/gpu/capture.hpp"
#include "xenon/gpu/command_processor.hpp"
#include "xenon/gpu/edram.hpp"
#include "xenon/gpu/ir.hpp"
#include "xenon/gpu/register_file.hpp"
#include "xenon/gpu/temporal_frame.hpp"
#include "xenon/memory/address_space.hpp"

namespace xenon::gpu {

class GraphicsSystem {
 public:
  explicit GraphicsSystem(memory::AddressSpace& memory);

  void reset();
  void submit_buffer(std::uint32_t physical_address, std::uint32_t dword_count);
  [[nodiscard]] std::uint32_t submit_ring(std::uint32_t physical_address,
                                          std::uint32_t capacity_dwords,
                                          std::uint32_t read_index,
                                          std::uint32_t write_index);
  // As submit_ring(), executing the work on `backend` in command order: the
  // graphics IR decoded so far is executed before each effect the guest can
  // observe (a fence or other memory write, an interrupt, a wait - see
  // CommandProcessor::set_guest_visible_barrier()). So when the guest sees a
  // fence pass the draws and resolves before it have executed, and the guest
  // memory they used may be reused. The rest executes before this returns,
  // or - pipelined drains (start_backend_thread()) - is left running on the
  // backend thread: the returned index is where decoding goes on. The read
  // pointer goes back to the guest as soon as the drain is decoded
  // (write_guest_ahead_of_work()), unless a draw of it reads command memory
  // (CommandProcessor::drain_reads_command_memory()): then behind its work
  // (write_guest_after_work()).
  [[nodiscard]] std::uint32_t submit_ring(Backend& backend, std::uint32_t physical_address,
                                          std::uint32_t capacity_dwords,
                                          std::uint32_t read_index,
                                          std::uint32_t write_index);
  [[nodiscard]] FrontendSubmissionCapture capture_buffer(
      std::uint32_t physical_address, std::uint32_t dword_count);
  [[nodiscard]] FrontendSubmissionCapture capture_ring(
      std::uint32_t physical_address, std::uint32_t capacity_dwords,
      std::uint32_t read_index, std::uint32_t write_index);
  // Creates a standalone capture by canonicalizing backend-authored memory,
  // snapshotting every guest range reachable by the normalized submission and
  // retaining the complete canonical EDRAM store.
  [[nodiscard]] bool capture_portable_buffer(
      Backend& backend, std::uint32_t physical_address, std::uint32_t dword_count,
      PortableSubmissionCapture& out, std::string* error = nullptr);
  [[nodiscard]] bool capture_portable_ring(
      Backend& backend, std::uint32_t physical_address,
      std::uint32_t capacity_dwords, std::uint32_t read_index,
      std::uint32_t write_index, PortableSubmissionCapture& out,
      std::string* error = nullptr);
  // Restores captured guest RAM/EDRAM through Memory V2 and replays normalized
  // work into the backend without depending on any prior backend cache state.
  [[nodiscard]] bool replay_portable_capture(
      Backend& backend, const PortableSubmissionCapture& capture,
      std::string* error = nullptr);
  // Replays captured normalized frontend work against the current guest RAM
  // and EDRAM. A complete register preamble is emitted first so replay is
  // deterministic even when the backend retained state from prior work.
  void replay_capture(Backend& backend,
                      const FrontendSubmissionCapture& capture);
  // Executes each pending graphics-IR command exactly once, in order, then
  // clears the pending stream. Backends retain their own shadow state across
  // submissions via RegisterWrite commands.
  // `submit` false records the IR without submitting the backend's batch
  // (Backend::end_submission_deferred()).
  // Hands the IR decoded so far to the backend: played on the backend thread
  // when one runs (start_backend_thread()), here otherwise. `submit` ends the
  // native batch; false leaves it open for the work that follows.
  void execute_ir(Backend& backend, bool submit = true);

  // GPU V3: consuming IR (native state, uploads, recording, submission),
  // publishing GPU writes and presenting run on a backend thread of their
  // own, in the order the frontend hands them over, while the command
  // processor decodes on. Every guest-visible effect still waits for the work
  // before it (see submit_ring(Backend&, ...)). Not started, all of it runs on
  // the calling thread, as tools and tests use it.
  void start_backend_thread();
  // Runs everything handed over, then joins the thread. Call before the
  // backend is destroyed.
  void stop_backend_thread() noexcept;
  [[nodiscard]] bool backend_thread_running() const noexcept { return executor_.running(); }
  // Queues `task` (which may use the backend) behind everything handed over.
  void post_backend_task(std::function<void()> task) { executor_.post(std::move(task)); }
  // Runs `task` behind everything handed over and waits for it.
  void run_backend_task(const std::function<void()>& task) { executor_.run(task); }
  // Writes `bytes` to guest physical memory once everything handed over has
  // executed - at once when it has (or no backend thread runs). Ordered with
  // the stream's own deferred writes, and waited for by the frontend's reads
  // of those bytes.
  void write_guest_after_work(std::uint32_t address, std::vector<std::byte> bytes);
  // Writes `bytes` to guest physical memory now, ahead of work still
  // executing: for state that describes the command processor's own fetch
  // position (the ring read pointer), not the results of the work. A write
  // to the same bytes still deferred behind work (write_guest_after_work())
  // keeps this one behind it, so the guest never sees the value go back.
  // Returns whether it was written now.
  bool write_guest_ahead_of_work(std::uint32_t address, std::span<const std::byte> bytes);
  // GPU V3 pipelined drains (default on with the backend thread;
  // XENON_GPU_PIPELINED_DRAINS=0 turns them off): a drain returns without
  // waiting for its last work, and the next one starts decoding beside it.
  [[nodiscard]] bool pipelined_drains() const noexcept {
    return pipelined_drains_ && executor_.running();
  }
  void set_pipelined_drains(bool enabled) noexcept { pipelined_drains_ = enabled; }
  [[nodiscard]] const BackendExecutor& backend_executor() const noexcept { return executor_; }
  // Explicit GPU -> CPU visibility hook for Memory v2 / CPU consumers.
  [[nodiscard]] bool make_guest_memory_cpu_visible(
      Backend& backend, std::uint32_t physical_address, std::uint32_t size);
  // Explicit native-owner -> canonical EDRAM checkpoint for backend switching,
  // capture/save-state and validation.
  [[nodiscard]] bool make_edram_canonical(Backend& backend);
  [[nodiscard]] PresentStatus present(Backend& backend,
                                      const PresentationFrame& frame);
  void set_interrupt_callback(
      std::function<void(std::uint32_t cpu_index)> callback) {
    command_processor_.set_interrupt_callback(std::move(callback));
  }
  // Called at each front-buffer swap, after the temporal analyzer (when
  // active) has closed the frame the swap shows.
  void set_swap_callback(std::function<void(const FrontBufferSwap&)> callback);

  // Temporal frame classification of the work execute_ir() runs (see
  // gpu/temporal_frame.hpp). Inactive until given a profile or put in
  // discovery mode; configure it before the GPU starts executing.
  [[nodiscard]] TemporalFrameAnalyzer& temporal() noexcept { return temporal_; }
  [[nodiscard]] const TemporalFrameAnalyzer& temporal() const noexcept { return temporal_; }

  [[nodiscard]] RegisterFile& registers() noexcept { return registers_; }
  [[nodiscard]] const RegisterFile& registers() const noexcept { return registers_; }
  [[nodiscard]] ir::Stream& stream() noexcept { return stream_; }
  [[nodiscard]] const ir::Stream& stream() const noexcept { return stream_; }
  [[nodiscard]] Edram& edram() noexcept { return edram_; }
  [[nodiscard]] const Edram& edram() const noexcept { return edram_; }
  [[nodiscard]] CommandProcessor& command_processor() noexcept { return command_processor_; }

 private:
  memory::AddressSpace& memory_;
  RegisterFile registers_{};
  ir::Stream stream_{};
  Edram edram_{};
  CommandProcessor command_processor_;
  TemporalFrameAnalyzer temporal_{};
  // CommandProcessor::Statistics::draws when execute_ir() last ran: a draw
  // (or resolve) has been decoded and not executed while they differ.
  std::uint64_t draws_at_last_execute_{};
  // Draws decoded since the last hand-off to the backend thread.
  std::uint32_t draws_since_hand_off_{};
  // Deferred guest memory writes still pending on the backend thread, in
  // posting order: the physical range and the task making it. The frontend
  // waits for the latest one overlapping guest memory it reads itself.
  struct DeferredWrite {
    std::uint32_t begin{};
    std::uint64_t end{};
    std::uint64_t sequence{};
  };
  std::deque<DeferredWrite> deferred_writes_{};
  // Defers a guest write behind everything handed over (see
  // write_guest_after_work()).
  void defer_guest_write(std::uint32_t address, std::function<void()> write, std::uint32_t size);
  bool pipelined_drains_{true};
  // Whether the current drain streams to the backend thread: it starts out
  // executing here and streams once it has decoded kStreamAfterDraws draws.
  static constexpr std::uint64_t kStreamAfterDraws = 8;
  bool drain_streaming_{};
  std::uint64_t drain_first_draw_{};
  [[nodiscard]] bool streaming();
  // Executes the IR decoded so far (only if a draw is among it, unless
  // `all`; `submit` ends the native batch), then `then`, behind everything
  // handed over, and returns once both have run.
  void run_ir_then(Backend& backend, bool all, bool submit, const std::function<void()>& then);
  // A guest memory write the command stream made, made now (`skip_if_unchanged`:
  // not when memory already holds the bytes).
  void make_guest_write(std::uint32_t address, std::span<const std::byte> bytes,
                        bool skip_if_unchanged);

  // Plays `commands` into `backend` (the body of execute_ir()).
  void consume_commands(Backend& backend, const std::vector<ir::Command>& commands,
                        bool submit, bool observe_temporal);
  void report_backend_errors();
  // Counts a frontend wait for a busy backend under `cause` (perf_frame.log).
  void note_hold(perf::Counter cause);
  [[nodiscard]] bool replay_portable_capture_now(Backend& backend,
                                                 const PortableSubmissionCapture& capture,
                                                 std::string* error);

  // Last member: destroyed (and drained) before the state its tasks use.
  BackendExecutor executor_{};
};

}  // namespace xenon::gpu
