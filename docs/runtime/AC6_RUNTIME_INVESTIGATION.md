# Ace Combat 6 runtime stall: structural notes

Status: **open**. This note was written during the structural refactor
(branch `development-restructure`). That pass deliberately changed no runtime
behaviour, so nothing here is a fix. It records who owns what in the
GPU/audio/guest-callback paths, which existing probes cover them, and which
structural properties deserve investigation first.

## Symptom

GPU and audio processing perform an initial pass, or a small number of
passes, and then useful activity stops. The cause is not established.

## Ownership map

All three paths live in `XenonSession` (`src/core/session/`). Each runs guest
code on its own `KernelThread`, with its own KPCR/static-TLS block from
`setup_guest_thread_tls_context()`, so a guest export called from inside a
callback resolves the correct current thread through
`ThreadManager::set_current_thread()`.

| Path | Thread / stack | Started by | Drives guest code through |
|------|----------------|------------|---------------------------|
| Main guest thread | `main_thread_`, 1 MiB stack | `start()` (`lifecycle/execution_control.cpp`) | `run_execution()` -> `dispatch_guest_thread()` |
| Created guest threads | one `KernelThread` each | `ExCreateThread` / `XamTaskSchedule` (`threading/thread_creation.cpp`) | `run_created_guest_thread()` |
| GPU pump | `gpu_pump_thread_`, 128 KiB callback stack from `init_gpu()` | `create_guest_process()` (`threading/guest_process.cpp`) | `run_gpu_pump_thread()` (`pumps/gpu_pump.cpp`) |
| Audio callbacks | `audio_thread_`, 128 KiB callback stack from `init_audio()` | `create_guest_process()` | `run_audio_callback_thread()` -> `AudioSystem::callback_pump()` (`src/audio/system.cpp`) |

Guest callbacks from both pumps go through `run_guest_callback()`, which calls
`dispatch_guest_thread()` with dispatch tracing off
(`execution/guest_dispatch.cpp`).

### GPU pump tick (every ~3 ms)

1. `submit_ring()` decodes PM4 between the kernel's ring read index and the
   write index, which `VdSwap` and `CP_RB_WPTR` stores through the register
   aperture both advance. It writes the read pointer back to the
   guest-supplied physical address.
2. `execute_ir()` plays the pending graphics IR into the native backend.
   `PM4_INTERRUPT` packets invoke the title's graphics interrupt (source 1)
   **synchronously on this thread**, through the callback installed in
   `init_gpu()`.
3. At a ~60 Hz accumulator it presents the front buffer, samples the
   title probes, and then runs the vsync interrupt callback (source 0)
   **synchronously on this thread**. Delivered and failed counts go to
   `gpu_interrupts_delivered_` / `gpu_interrupts_failed_`.

### Audio callback pump (every <= 2 ms)

`AudioSystem::callback_pump()` invokes each render client's guest callback
only while that client has **callback credits**:

- A client starts with `kMaxQueuedRenderFrames` (8) credits.
- Each callback spends one credit.
- A credit is returned only when the host mixer (`AudioSystem::render()`)
  finishes consuming a frame that the guest submitted with
  `XAudioSubmitRenderDriverFrame`.

## Structural properties to investigate first

1. **Audio stops after eight callbacks if the guest submits nothing.** If the
   title's render callback runs but does not submit a frame, no credit ever
   comes back and the pump stops calling it after exactly eight invocations.
   That matches "an initial number of passes". A submission rejected by
   `submit_render_frame()` has the same effect: invalid handle, untranslatable
   frame, suspended client or full queue.
2. **Pump callbacks are synchronous and unbounded.** A guest ISR or render
   callback that blocks in a kernel wait or spins on a lock stops its entire
   pump. A stalled vsync callback also stops ring draining and presentation.
   There is no watchdog or timeout. The stop-time report (`stop()` ->
   `report_stop_diagnostics()`) lists threads blocked inside kernel calls and
   threads running guest code, which tells these cases apart.
3. **GPU-CPU handshakes through memory.** The stop report prints the
   `WAIT_REG_MEM` fence the command processor is parked on and the
   surrounding indirect-buffer words. The vsync ISR probe comments note that
   the guest's per-tick chain only signals when two counters at
   `ctx+0x412C`/`ctx+0x4130` differ.
4. **Kernel event state read once.** The handshake-event probe tests whether
   the title signals two render-setup events by writing `SignalState`
   directly, rather than by calling `KeSetEvent`. The comment there records
   that the host-side `KernelEvent` reads that field only once, at first
   resolution.
5. **Thread identity.** `anonymous_thread_export_diag.log` records exports
   issued by a host thread with no registered `KernelThread`. These break
   thread-keyed semantics such as mutant ownership and waits.

## Existing probes

The probes are unconditional `*_diag.log` files in the working directory,
written through `logging::append_probe_log()`. The title-specific probes are
isolated in `src/core/session/diagnostics/title_probes.cpp` and are called
from the same points as before the refactor.

| File | Where | Records |
|------|-------|---------|
| `event_dispatch_823AD848_diag.log` | GPU pump, per vsync (title) | event-dispatcher bitmask/counter changes |
| `subsystem_label_diag.log` | GPU pump, once (title) | label string of the subsystem that owns the stuck events |
| `handshake_event_raw_memory_diag.log` | GPU pump, per vsync (title) | raw `SignalState` of the two handshake events |
| `vsync_isr_obj_chain_diag.log` | GPU pump, per vsync (title) | the vsync ISR's counter/gate words |
| `ex_create_thread_caller_diag.log` | ExCreateThread (title) | callers creating the 0x821EEDE0 render-setup thread |
| `vsync_tick_diag.log`, `vsync_cb_diag.log`, `tick_phase_diag.log` | GPU pump | vsync ticks, callback timing, slow ring/IR/present phases |
| `all_threads_export_trace_diag.log` | GPU pump, every ~30 s | each thread's recent export calls |
| `audio_callback_diag.log` | audio callbacks | callback entry/return, first 20 then every 500th |
| `thread_identity_diag.log`, `thread_start_diag.log`, `set_current_thread_diag.log`, `thread_create_flags_diag.log` | thread creation/start | thread ids, start addresses, suspend flags |
| `dispatch_lookup_diag.log`, `dispatch_outcome_diag.log` | guest dispatch | compiled-entry lookups and per-thread dispatch outcomes |
| `anonymous_thread_export_diag.log`, `unique_exports_seen_diag.log` | export dispatch | exports with no thread identity; first use of each export |

Other subsystems still write raw `fopen` probes. They should move to
`append_probe_log()` when their libraries link `xenon_logging`:

- kernel: `wptr`/`rptr`/`ring_init`, `thread_resume`, `thread_main_entry`, `nt_read_file`
- xbox exports: `nt_set_event`, `ke_wait`, `set_event_all`, critical-section and spin-lock contention, `vdswap_calls`, `vsync_callback`
- memory: physical/virtual allocation failures
- CPU: `signal_write_trap`
- graphics: `draw_calls`, `truncation`

In the PM4 interrupt path, the session also logs the title's interrupt
handler words to the console (first four deliveries, verbose logging only).

## Suggested order for the debugging phase

1. Confirm whether the audio client exhausts its eight credits: count
   callbacks against successful `submit_render_frame()` calls.
2. Use the stop report to see whether the GPU pump thread is still ticking,
   is inside the vsync callback, or is parked behind a `WAIT_REG_MEM` fence.
3. Correlate (2) with the handshake-event and ISR-counter probes to find the
   first producer that stops advancing.

Any fix belongs in generic runtime code with its own tests. Title-specific
probes stay in `title_probes.cpp` and are removed once the cause is found.
