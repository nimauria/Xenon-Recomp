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
1. Parse and load XEX into Memory V2
2. Resolve imports against export registry
3. Bind precompiled native code (if any)
4. Initialize guest process and main thread
5. Set up TLS, stack, entry point

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

### In Progress / Known Gaps

- 🔄 Real graphics backend selection and window/swapchain presentation
  (`init_gpu()` always creates `NullBackend` regardless of the requested
  renderer)
- 🔄 Cooperative pause/stop checkpoints in generated code (today, stop is
  cooperative-then-hard-kill at the process level; pause only changes
  reported state)
- 🔄 TLS setup (stack allocation exists; TLS slot/index wiring does not yet)

### TODO

- ⏳ Register xboxkrnl exports (filesystem, threading, synchronization)
- ⏳ Register XAM exports (input, user profiles, achievements) — note some
  Content Services V1 export/result-enum wiring in `src/xam/` currently has
  pre-existing compile errors unrelated to session/runtime-host work; see
  the ordinal gaps in `xam_content_exports.cpp` and the `result` enum in
  `xenon/xam/types.hpp`
- ⏳ Thread management (multiple guest threads)
- ⏳ Audio subsystem integration
- ⏳ Network subsystem integration
- ⏳ Save state / restore

## See Also

- [Runtime Host](RUNTIME_HOST.md)
- [CPU V2 Design](CPU_V2_DESIGN.md)
- [Memory V2](MEMORY_V2.md)
- [XEX Loader](XEX_LOADER.md)
- [GPU V1](GPU_V1.md)
