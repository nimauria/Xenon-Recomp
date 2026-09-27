# Xenon Runtime Session V1

## Overview

The Xenon Runtime Session is a unified coordination layer that owns and connects all low-level subsystems required to execute Xbox 360 games. It replaces manual subsystem instantiation with a coherent lifecycle that ensures proper initialization order, dependency management, and clean shutdown.

## Purpose

Before the Runtime Session, the launcher manually instantiated random runtime pieces without clear ownership or lifecycle management. The Runtime Session provides:

- **Single Point of Coordination**: One class that owns Memory V2, CPU V2, GPU, filesystem, kernel, input, and loaded XEX
- **Clear Lifecycle**: Explicit initialization → load → run → shutdown flow
- **Unified Export System**: Common Xbox export registry replacing subsystem-specific import dispatchers
- **Common Call Bridge**: Single ABI layer for all Xbox system calls
- **Proper Ownership**: No global singletons; explicit subsystem lifetime management

## Architecture

### XenonSession

`xenon::core::XenonSession` is the main runtime session class. It:

- Implements `cpu::RuntimeServices` for CPU integration
- Owns all major subsystems
- Manages the export registry
- Provides the main execution loop infrastructure

### Owned Subsystems

The session owns and coordinates:

| Subsystem | Type | Purpose |
|-----------|------|---------|
| Memory | `memory::AddressSpace` | Xbox 360 physical and virtual memory |
| CPU | `cpu::ExecutableCodeCache` | Native code translation cache |
| GPU | `gpu::Backend` | Graphics backend (Vulkan/D3D12/Null) |
| Filesystem | `filesystem::VirtualFileSystem` | Guest filesystem abstraction |
| Kernel I/O | `kernel::KernelIoManager` | File operations and handles |
| Input | `input::InputSystem` | Controller/keyboard/mouse |
| Loaded XEX | `xbox::LoadedXex` | Currently executing game |

### Export Registry

`xenon::core::ExportRegistry` replaces subsystem-specific import dispatchers:

```cpp
struct ExportDescriptor {
  std::string library;           // "xboxkrnl", "xam", etc.
  std::string name;              // Export name (if known)
  std::uint32_t ordinal;         // Export ordinal
  ExportHandler handler;         // Implementation callback
  ExportRequirement requirement; // Required, Stubbed, Optional, DiagnosticOnly
};
```

Supported libraries:
- `xboxkrnl.exe` (kernel services)
- `xam.xex` (Xbox Application Management)
- `xbdm.xex` (if needed for debug features)
- Future: audio services, network services

### Call Bridge

`xenon::core::CallBridge` provides a common ABI layer:

- **PPC Argument Extraction**: Reads r3-r10 and stack arguments
- **Return Values**: Sets r3 (32-bit) or r3:r4 (64-bit)
- **Guest Memory Access**: Big-endian reads/writes with fault handling
- **String Marshaling**: Null-terminated and wide-string support

## Lifecycle

### 1. Create Session

```cpp
auto session = std::make_unique<xenon::core::XenonSession>();
```

### 2. Initialize

```cpp
xenon::core::SessionConfig config{};
config.enable_graphics = true;
config.enable_input = true;
config.graphics_backend = "d3d12";

auto result = session->initialize(config);
if (!result.success) {
  // Handle error
}
```

Initialization order:
1. Memory V2
2. Filesystem
3. Kernel I/O
4. CPU code cache
5. GPU (if enabled)
6. Input (if enabled)
7. Export registry

### 3. Mount Content

```cpp
session->mount_content("/path/to/game", "game:");
```

### 4. Load Game

```cpp
std::vector<std::byte> xex_bytes = load_file("game.xex");
auto result = session->load_game(xex_bytes, "DEADBEEF");
```

Loading steps:
1. Parse the base XEX (`xbox::parse_xex_image()`)
2. If a title update was selected, apply it (`xbox::apply_title_update()` -
   see "Title Update Integration" below) to produce the *effective* image
3. Map the effective image into Memory V2 (`xbox::map_xex_image()`)
4. Resolve imports against export registry
5. Bind precompiled native code (if any) - rejected if it declares
   incompatibility with the effective image's identity (see below)
6. Initialize guest process and main thread
7. Set up TLS, stack, entry point

### Title Update Integration

`xex_bytes` passed to `load_game()` is always the immutable base executable.
An optional third argument, `title_update_bytes`, carries the raw bytes of a
selected XEXP title update:

```cpp
auto result = session->load_game(xex_bytes, "DEADBEEF", title_update_bytes);
```

When non-empty, `load_game()` validates and applies it via
`xbox::apply_title_update()` - XEX Loader V2's canonical XEXP patcher, the
*only* place patch semantics are implemented (see `docs/xbox/XEX_LOADER_V2.md`) -
before anything is mapped into memory. The resulting **effective image**, not
the base image, is what actually gets mapped, import-resolved, and executed.
A malformed or incompatible update (wrong title/media ID, failed base-
signature/version validation, corrupt patch records) fails `load_game()`
outright; there is no silent fallback to the base XEX. An empty (default)
`title_update_bytes` preserves the base-only launch path unchanged - a title
update is never required.

`xenon_runtime_host` (see `docs/runtime/RUNTIME_HOST.md`) is the production caller:
it reads `session->content_graph()->selected_title_update` - the one
deterministic selection `ContentManager::build_content_graph()` already made
during `mount_content_graph()` - reads that file's bytes, and passes them
into `load_game()`. It never rediscovers or re-selects an update itself;
Content Services remains the single owner of "which update is this launch
using."

**Effective executable identity.** After a successful `load_game()`,
`session->effective_identity()` returns an `xbox::XexEffectiveIdentity`:
title/media ID, the base image's own version, the effective (possibly
patched) image's version, a SHA1 hash of the effective image's bytes
(`xbox::compute_effective_image_hash()`), and whether a title update was
applied. This is the identity module-compatibility validation and
diagnostics (`status.json`'s `loadedXex` fields) compare against - never the
base XEX's own identity once a title update has changed the running code.

**Module compatibility.** A native extension may optionally export
`const char* Xenon_SupportedExecutableRevisions()` (semicolon-separated,
lowercase-hex SHA1 hashes) declaring which effective-image revision(s) its
compiled registry is valid for. `load_native_extension()` rejects binding a
module that declares revisions not including the currently loaded effective
image's hash - a module compiled for the base executable is refused against
a title-update launch, and vice versa, even though both share the same title
ID. A module that exports nothing (or an empty string) is not checked, for
back-compat with modules that predate this contract.
`xenon::recomp::generate_project()` (the Recomp Driver) emits this export
automatically from whatever `XexImage` it analyzed, so a normally-built game
module always declares the exact revision it was generated from.

### 5. Start Execution

```cpp
auto result = session->start();
```

### 6. Shutdown

```cpp
session->shutdown();
```

Shutdown order (reverse of initialization):
1. Input system
2. GPU backend
3. CPU code cache
4. Kernel I/O
5. Filesystem
6. Memory V2

## Export Registration

### Registering Exports

```cpp
ExportDescriptor desc{};
desc.library = "xboxkrnl";
desc.name = "NtCreateFile";
desc.ordinal = 0x00D2;
desc.handler = [](ExportCallContext& ctx) -> bool {
  CallBridge bridge(ctx.cpu, ctx.memory);
  auto handle = bridge.read_pointer(0);
  // ... implementation
  bridge.set_u32_result(STATUS_SUCCESS);
  return true;
};
desc.requirement = ExportRequirement::Required;

session->exports()->register_export(std::move(desc));
```

### Export Requirements

- **Required**: Must be implemented; fatal if missing during import resolution
- **Stubbed**: Placeholder that returns success or neutral state
- **Optional**: Game may not use it; logged but not fatal
- **DiagnosticOnly**: Logged for analysis; does not affect execution

### Unknown Export Handling

When an unknown export is called:

1. Check the export registry
2. If not found and `enable_export_diagnostics` is true:
   - Log library, ordinal, call address, thread ID
   - Check requirement classification
3. Never silently return success for required exports
4. Return `false` to signal "not handled"

## Current Status

### Implemented

- ✅ `XenonSession` core structure
- ✅ `ExportRegistry` for unified export handling
- ✅ `CallBridge` for PPC ABI
- ✅ Lifecycle: initialize → load → start → stop → shutdown
- ✅ Subsystem ownership (Memory, CPU, GPU, Filesystem, Kernel, Input)
- ✅ XEX loading from file bytes, with real title-ID extraction for content
  mounting (previously always 0)
- ✅ Import resolution as a diagnostic pass against the export registry
  (`unresolved_imports()`), non-fatal
- ✅ Native extension (module compiled-code) dynamic loading and binding
  into a CPU V2 `ExecutionContext` — see [Runtime Host](RUNTIME_HOST.md) for
  the export contract
- ✅ Guest stack allocation and main-thread `CpuState` setup
- ✅ Real execution: `start()` spawns a dedicated thread that looks up and
  invokes the bound registry's entry point; `state()`/`last_error()` are
  thread-safe for a supervising process to poll
- ✅ Generic runtime/game-host process (`xenon_runtime_host`) that owns the
  only `XenonSession` instance; the launcher's `RuntimeBridge` supervises it
  by file (status/log/stop), never executes guest code itself

### Now Real (XenonSession Production Integration V1)

- ✅ `init_gpu()` selects and constructs a real native backend (Vulkan/D3D12,
  whichever the build compiled in) based on `graphics_backend` -
  "Automatic"/"Vulkan"/"D3D12" either succeed with that real backend or fail
  initialization outright; `gpu::NullBackend` is only ever constructed for an
  explicit "Null"/"None" request
- ✅ `init_input()` selects and constructs real SDL/XInput drivers based on
  `input_drivers`; a normal (non-explicit-null) request that cannot create
  any real provider fails initialization rather than running with zero
  drivers
- ✅ Audio V1 is a real, always-on subsystem for normal Play: `init_audio()`
  fails session initialization if the host backend cannot be created, and
  `audio_master_volume`/`audio_mute_unfocused` are genuinely consumed (see
  [Runtime Host](RUNTIME_HOST.md))
- ✅ xboxkrnl exports registered (filesystem I/O: `NtCreateFile`,
  `NtReadFile`, `NtWriteFile`, `NtOpenFile`, `NtQueryDirectoryFile`,
  `NtQueryInformationFile`, `NtSetInformationFile`,
  `NtQueryVolumeInformationFile`, `NtQueryFullAttributesFile`,
  `NtFlushBuffersFile`, `NtReadFileScatter`) — bridged from the existing
  `xbox::ImportRegistry`/`GuestIoBridge` (previously only exercised by
  `tests/xbox/xbox_import_tests.cpp` in isolation, never constructed by any
  session) into the same canonical `export_registry_` XAM/Audio use
- ✅ XAM input exports (`XamInputGetCapabilities`/`GetState`/`SetState`/
  `GetKeystroke`/`GetKeystrokeEx`/`GetCapabilitiesEx`) registered into
  `export_registry_` too, via `input::xam::guest::GuestInputBridge` -
  previously only targeted the separate `cpu::ExternalCallRegistry`, which no
  session ever populated
- ✅ Content Services V1's export/result-enum wiring in `src/xam/` compiles
  cleanly; the `result`/ordinal gaps this doc previously called out are gone

### Now Real (XenonSession Production Integration V1 — final completion pass)

- ✅ Real guest process/thread model: `create_guest_process()` builds a
  `kernel::KernelProcess` (wrapping this same session's Memory V2 instance
  via `kernel::KernelMemory`, not a second address space), registers the
  loaded XEX as a `kernel::KernelModule` (base/size/entry/TLS info consumed
  directly from XEX Loader V2's output - not reparsed), and `start()` creates
  a real `kernel::KernelThread` via `ThreadManager::create_thread()` whose
  `ThreadEntry` is `run_execution()`. The chain is now `XenonSession ->
  KernelProcess -> KernelThread -> CPU V2 -> compiled native guest code`, not
  a bare `CpuState`. A `KernelThread` cannot restart after terminating, so
  each `start()` call creates a fresh thread object against the same
  process/module/TLS state `create_guest_process()` set up once at
  `load_game()` time (matching real Xbox process/thread separation, and the
  previous bare-`std::thread` model's "each start() gets a runnable thread"
  behavior).
- ✅ TLS is real: `include/xenon/core/guest_thread_context.hpp` +
  `src/core/guest_thread_context.cpp` allocate a per-thread KPCR block and a
  compiler-emitted static-TLS block from XEX Loader V2's already-parsed
  `XexTls` (raw template copied, remainder zero-filled), and
  `create_guest_process()` points the main thread's `CpuState::gpr[13]` at
  the KPCR - the same register real recompiled PPC code already expects to
  hold a KPCR pointer (this needs no lifter/codegen changes: static
  recompilation carries the original r13-relative loads/stores forward
  unchanged). KPCR field offsets (`tls_ptr`, self-pointer, stack base/limit)
  are independently laid out to match real Xbox 360 kernel semantics
  (xenia-project/xenia's `X_KPCR` used as the research reference per
  CLAUDE.md, not copied). Two threads sharing one `XexTls` template get
  fully independent, correctly-primed TLS instances - proven directly in
  `tests/core/session_tests.cpp` without needing two full guest entry
  points. The one KPCR field intentionally left zero is `current_thread`
  (offset 0x100): Xenon has no guest-visible `KTHREAD` struct to point it
  at, and nothing else in Xenon reads it back through guest memory (only
  through the host-side `kernel::ThreadManager::current_thread()`) - a
  narrow, documented scope note, not a silent stub.
- ✅ Exception context: `run_execution()` now runs with
  `ThreadManager::set_current_thread()` established for the real
  `KernelThread`, and a `memory::MemoryFault` thrown by a genuine Memory V2
  guest-memory fault (or a CPU trap) is converted via
  `kernel::ExceptionDispatcher::fault_to_exception()` and dispatched through
  a real `kernel::ExceptionDispatcher` (a default handler logs the
  structured record) instead of only reaching a stringified `last_error()`.
- ✅ Real presentation host: `runtime_host/src/presentation_host.{hpp,cpp}`
  create an SDL2 window (SDL2 is already an Xenon dependency for input/
  audio - no second window/UI toolkit added) whenever `session.gpu()` is a
  real backend (Vulkan or D3D12; never for `NullBackend`/headless), and wire
  it to the already-implemented `configure_presentation()` on whichever
  concrete backend is active (`SDL_Vulkan_CreateSurface` for Vulkan,
  `SDL_GetWindowWMInfo`'s HWND for D3D12 - no second presentation stack).
  The main supervisory loop in `runtime_host/src/main.cpp` pumps SDL events:
  window close requests an orderly `session.stop()` (not an immediate
  process kill - the existing hard-stop grace-period path still applies if
  the game does not cooperate), resize calls `resize_presentation()`
  directly, and focus changes call `session.set_focused()` (which now has a
  real automatic caller, closing the gap the previous pass left - see
  `set_focused()`'s effect on Audio/Input below).
- ✅ `session.set_focused(bool)` is no longer dormant: the presentation
  host's window focus events call it, which mutes `AudioSystem` when
  `audio_mute_unfocused` was requested and forwards to
  `InputSystem::set_focused()`.
- ✅ **Guest export ABI round trip, proven through actually-recompiled PPC
  code** (`tests/core/guest_export_abi_tests.cpp`), not a direct C++
  shortcut. The real mechanism, discovered by tracing the recomp driver
  rather than inventing a format: an XEX import's `guest_thunk` (see
  `XexImport`, populated by `parse_native_import_libraries()`) is the guest
  address of a plain **data** placeholder word (ordinal in the low 16 bits),
  never real PPC instructions - so it can never become a discovered/compiled
  function, and a guest `bl guest_thunk` therefore *always* lowers through
  CPU V2's existing `Op::Call` fallback (`backend_cpp_aot.cpp`:
  `runtime.call(target,...)`, unchanged) to `XenonSession::call()`. That
  fallback now checks `target` against `loaded_xex_->image.imports[i]
  .guest_thunk` and, on a match, calls `external_call(module, ordinal,
  state, memory)` - reaching the exact same `core::ExportRegistry` every
  other subsystem's exports use. **No recomp-driver analysis or CPU V2
  codegen change was needed for this** - it works identically for every
  title, not just the test fixture.
  Building the proof surfaced two real, previously-invisible defects, fixed
  along the way (both only matter once something actually *links* the
  generated code into an executable/shared library, which no prior test
  did):
  - `generate_project()`'s `registry.cpp` declared each compiled function's
    `extern` forward declaration inside `namespace xenon::recomp {}`, while
    the function shards (`functions/shard_*.cpp`) define them at **global**
    scope - a real symbol-name mismatch, invisible as long as `xenon_game`
    only ever built as a static library (archiving never resolves external
    references). Fixed by moving the `extern` declarations to global scope.
  - The recomp driver's generated project only ever produced `xenon_game`
    as a `STATIC` library - not loadable via `XenonSession::
    load_native_extension()`'s `LoadLibrary`/`dlopen`. `generate_project()`
    now also emits `module_export.cpp` + a `xenon_game_module` `SHARED`
    target wrapping the same generated code (`xenon_game` stays the
    reusable static artifact - nothing is recompiled twice) and exporting
    the canonical `Xenon_BindCompiledRegistry(ExecutionContext&)` ABI a real
    game module must provide.
  Two calls are proven end to end (XEX Loader V2 -> Recomp Driver -> CPU V2
  codegen -> compiled `xenon_game_module.dll` -> real `XenonSession` ->
  `KernelProcess`/`KernelThread` -> the `bl`/import/ExportRegistry path
  above -> the live subsystem), each verifying real PPC ABI marshalling
  (argument registers, guest pointers, return value in r3, guest memory
  writes and the absence of writes on failure): `XAudioGetUnderrunCount`
  (audio exports register under the `"xboxkrnl"` library string - see
  `src/audio/exports.cpp`) and `XamInputGetCapabilities` (`"xam"`). A real
  xboxkrnl *file I/O* round trip (`NtCreateFile`/`NtReadFile`/...) was not
  attempted: those need a guest `OBJECT_ATTRIBUTES`/`UNICODE_STRING`
  structure built in guest memory, separate marshalling work from the
  import-bridge mechanism itself, which is identical for every library/
  ordinal and is what this test proves.

### Now Real (XenonSession Production Integration V1 — closure pass)

This pass closed the validation gaps the previous ABI round-trip pass left
open: all three system library classes are now proven (including a
*structured* xboxkrnl call, not just the audio zero-argument one), negative
import paths fail explicitly instead of silently, an import-identity
regression guards `core::ExportRegistry` against cross-library ordinal
collisions, `create_guest_process()` no longer leaks on a partial failure,
the generated game module's shared *and* static link targets are both
proven, and two real defects this work surfaced were fixed.

- ✅ **Two real defects found and fixed while proving the negative paths**,
  both in the exact same "call resolves to nothing runnable" family the
  previous pass's `guest_thunk` fix touched:
  - `Op::Call` (`backend_cpp_aot.cpp`, a direct `bl` to a compile-time-
    constant target) never tried the local CPU V2 compiled registry at all
    - only `CallIndirect`/linked `BranchIndirect` did. Since there is no
      CPU-v1 `ExecutableCodeCache` registration for a native-extension-
      loaded title, a direct `bl` to *another locally compiled function in
      the same module* always fell to `runtime.call()`, found nothing,
      wasn't a recognized import either, and got back a non-terminal
      `Branch` that `Op::Call`'s own `if(rr.terminal())` check silently
      swallowed - the callee simply never ran, with no error of any kind.
      Fixed by trying `context.lookup_compiled(target, Call)` first, exactly
      mirroring the pattern the indirect-call paths already used. Proven by
      `guest_export_abi_tests.cpp`'s `local_add_one` call (41 -> 42).
  - `XenonSession::call()`'s two failure fallbacks (a recognized import
    whose export this build does not implement; a target that is neither
    compiled code nor a known import at all) both returned a non-terminal
    `{Branch, target, 0}` - silently swallowed by `Op::Call`/`CallIndirect`
    for the same reason as above, letting execution continue with a stale,
    unspecified r3 as though the call had quietly succeeded. Both now return
    a terminal `{Trap, target, ExceptionCode::ProcedureNotFound}`, which
    propagates out through the same codegen `if(rr.terminal())` checks and
    reaches `run_execution()`'s existing `kernel::ExceptionDispatcher`
    path - an explicit, diagnosable failure instead of a silent no-op.
- ✅ `xex_loader.cpp`'s `parse_native_import_libraries()`: an import table
  entry whose thunk address falls outside the effective image (a malformed/
  corrupt entry) used to fall through silently and push a plausible-looking
  `XexImport{ordinal=0, attributes=0}` - now skipped outright, matching the
  existing `thunk_rva_or_addr == 0 -> continue` precedent, instead of
  fabricating data that could accidentally resolve against whatever real
  export happens to sit at ordinal 0 for that library.
- ✅ `create_guest_process()` no longer leaks on a partial failure: if
  `setup_guest_thread_tls_context()` (or module registration) fails *after*
  the main thread's guest stack and `kernel_process_`/`kernel_memory_` were
  already created, the new `release_partial_guest_process()` releases the
  stack allocation and resets both, instead of leaving them alive with
  nothing meaningful referencing them. Proven with a synthetic XEX
  advertising an unsatisfiable (~4 GiB) TLS block size - deterministic
  regardless of host memory pressure - verifying `kernel_process()` is
  `nullptr` afterward and that the session remains fully reusable through
  its documented `shutdown()` -> `initialize()` cycle.
- ✅ All three system library classes are now proven through actually-
  recompiled PPC in one fixture (`guest_export_abi_tests.cpp`):
  `XAudioGetUnderrunCount` (Audio V1, registered under `"xboxkrnl"`),
  `XamInputGetCapabilities` (`"xam"`), and - new this pass - **`NtCreateFile`**
  (`"xboxkrnl"`, ordinal `0x00D2`), the structured ABI call: a real guest
  `OBJECT_ATTRIBUTES` (root_directory/name/attributes) pointing at a real
  `ANSI_STRING` (length/max_length/buffer) over real path bytes, a real
  `IO_STATUS_BLOCK` output, a real output `Handle` write, and - because
  `NtCreateFile` takes 9 parameters - a real PPC ABI **stack-passed**
  argument (`create_options`, written to and read back from `r1+0x54`) in
  addition to `r3..r10`. Routed through the exact same production
  `GuestIoBridge`/`IoFacade`/`KernelIoManager` every other xboxkrnl file-I/O
  caller uses (`xboxkrnl_io_exports.cpp`'s `NtCreateFile_thunk`); no
  filesystem/content is mounted, so the call deterministically returns a
  real non-success NTSTATUS, proving the marshalling rather than a
  coincidental success.
- ✅ Negative import-resolution paths (`guest_import_negative_tests.cpp`),
  proven through the production import-resolution boundary
  (`XenonSession::call()`/`load_game()`'s diagnostics/`xbox::parse_xex_image()`
  directly, as the task's own wording allows as an alternative to a second
  full compiled-module build): an unknown library, a real library with an
  ordinal nothing registers, a malformed native import table entry, and a
  call target that is neither compiled code nor a known import all now fail
  explicitly (never "return zero and continue" or silently dispatch to the
  wrong export) - and a missing/incompatible native game module path
  (`start()` refusing to run without a bound registry) is regression-tested
  directly.
- ✅ Import identity regression: `core::ExportRegistry`'s ordinal/name keys
  are already library-scoped (`"<library>:<ordinal>"`), so the same ordinal
  registered under `xboxkrnl`/`xam`/a third library never cross-resolves -
  proven directly rather than only inferred from reading the source.
- ✅ Generated game module regression: `guest_export_abi_tests.cpp` now
  builds **both** the `xenon_game_module` `SHARED` target and the
  `xenon_game` `STATIC` target from the recomp driver's output in the same
  run, proving both actually link from the identical generated shard files
  (the shared module's link remains the authoritative registry-symbol-
  resolution test the previous pass's namespace-mismatch bug needed; static-
  only archiving is what let that bug hide originally).
- ✅ Presentation ownership rollback
  (`tests/runtime_host/presentation_host_tests.cpp`, new `xenon_
  presentation_host_tests` target): `PresentationHost::create()` already
  performed full, real cleanup on every internal failure branch (destroy the
  window, `SDL_QuitSubSystem`, never publish `backend_`) - this is now
  exercised for real via the always-reachable "no presentation path for this
  backend" rejection (`NullBackend`), proving no leaked window/stuck SDL
  subsystem reference and that the object remains usable afterward, without
  needing a real GPU device to inject the failure deterministically.

### In Progress / Known Gaps

- 🔄 Cooperative pause/stop checkpoints in generated code (today, stop is
  cooperative-then-hard-kill at the process level; pause only changes
  reported state)
- 🔄 A real xboxkrnl file I/O *success* round trip (`NtCreateFile` actually
  opening/creating a file through a mounted filesystem device) is still not
  attempted - the closure pass proved the structured ABI marshalling through
  a deterministic failure path (no content mounted); a success path is the
  same mechanism plus real content-graph mounting, not additional
  marshalling work.
- 🔄 `KernelThread`/`ModuleManager`/`ThreadManager` creation itself cannot
  currently fail in this codebase (`ModuleManager::load_module()` and
  `ThreadManager::create_thread()` are unconditional), so
  `create_guest_process()`'s corresponding `if (!module) {...}` /
  `start()`'s `if (!main_thread_) {...}` branches remain defensive but
  untested dead code; only the two failure points that genuinely can occur
  today (guest stack/TLS allocation exhaustion) have rollback coverage.
- 🔄 Presentation rollback coverage stops at the backend-rejection path
  (deterministic, needs no GPU device); the deeper `SDL_CreateWindow`/
  `SDL_Vulkan_CreateSurface`/`configure_presentation()` failure branches
  inside `PresentationHost::create()` are real and already clean up
  correctly (see the source), but injecting *those* specific failures
  deterministically would need a real, uninitialized GPU backend object,
  which risks undefined behavior in third-party SDL/Vulkan calls rather than
  a clean, portable test.

### TODO

- ⏳ Thread management (multiple guest threads)
- ⏳ Network subsystem integration
- ⏳ Save state / restore

## See Also

- [Runtime Host](RUNTIME_HOST.md)
- [CPU V2 Design](../cpu/CPU_V2_DESIGN.md)
- [Memory V2](../memory/MEMORY_V2.md)
- [XEX Loader](../xbox/XEX_LOADER.md)
- [GPU V1](../graphics/GPU_V1.md)
