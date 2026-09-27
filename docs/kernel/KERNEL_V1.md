# PROJECT XENON — KERNEL V1 EXECUTION ENVIRONMENT

## Overview

Kernel V1 provides a comprehensive Xbox 360 kernel execution environment that translates observable Xbox kernel behavior into native runtime services. It builds upon the existing filesystem and I/O infrastructure to provide threads, synchronization, time services, memory management, module loading, and exception handling.

## Design Philosophy

**Do not emulate an Xbox kernel scheduler at cycle level.**

Instead, Kernel V1:
- Maps guest threads efficiently onto host threads initially (unless profiling shows another scheduler is required)
- Translates Xbox kernel semantics to native OS primitives where possible
- Provides accurate observable behavior for synchronization and timing
- Integrates tightly with Memory V2 for guest address space management
- Connects CPU/Memory faults to guest exception handling

## Architecture

### Component Hierarchy

```
┌─────────────────────────────────────────────────────────────┐
│                    KernelProcess                            │
│  ┌────────────────┬──────────────────┬───────────────────┐ │
│  │ ThreadManager  │  ModuleManager   │  KernelMemory     │ │
│  └────────────────┴──────────────────┴───────────────────┘ │
└─────────────────────────────────────────────────────────────┘
                            │
        ┌───────────────────┼───────────────────┐
        │                   │                   │
   HandleTable        IoManager         ExceptionDispatcher
        │                   │                   │
    KernelObject      FileSystem            Memory V2
        │
   ┌────┴────┬─────┬──────┬────────┬────────┐
   │         │     │      │        │        │
 Thread   Event  File  Semaphore Mutant  Timer
```

## Object System

### KernelObject Base Class

All kernel objects inherit from `KernelObject`:

```cpp
class KernelObject {
 public:
  ObjectType type() const noexcept;
  std::uint64_t object_id() const noexcept;
  std::uint32_t handle_count() const noexcept;
  
 protected:
  explicit KernelObject(ObjectType type);
  virtual void on_last_handle_closed() noexcept;
};
```

### Object Types

```cpp
enum class ObjectType : std::uint8_t {
  Unknown, File, Event, IoCompletionPort, Thread,
  Semaphore, Mutant, Timer, Process, Module,
};
```

**Lifetime:** Objects are kept alive by handle references and `shared_ptr` ownership. When the last handle is closed, `on_last_handle_closed()` is called before destruction.

## Threads

### KernelThread

Represents an executing Xbox 360 thread mapped to a host `std::thread`.

**Key Features:**
- Thread IDs, priorities (Idle→TimeCritical), processor affinity
- Suspend/resume with suspend count tracking
- 64 TLS slots per thread
- Thread-local kernel state pointer
- Exit codes and termination

**Implementation:** 1:1 mapping to `std::thread` with `std::mutex` + `std::condition_variable` for state management.

### ThreadManager

Process-wide thread registry with `thread_local` current thread tracking.

## Synchronization

### Events
Manual reset or auto-reset signaling with `std::condition_variable`.

### Semaphores
Counting semaphore with maximum count validation.

### Mutants (Mutexes)
Recursive mutexes with Xbox abandon semantics when owner thread terminates.

### Timers
Notification or synchronization timers with due time and optional period.

### Wait Functions
`wait_for_single_object()` and `wait_for_multiple_objects()` support waitable object types (Event, Thread, Semaphore, Mutant, Timer).

## Time Services

```cpp
class TimeServices {
  static std::uint64_t system_time();              // FILETIME format
  static std::uint64_t performance_counter();       // High-resolution ticks
  static std::uint64_t performance_frequency();     // Ticks per second
  static void sleep(std::uint32_t milliseconds);
};
```

**Deterministic monotonic behavior** via C++ standard time facilities.

## Memory Integration

`KernelMemory` connects xboxkrnl memory exports to Memory V2 `AddressSpace`:

- Virtual memory: allocate, free, protect, query
- Physical memory: allocate, free, map to virtual
- Read/write helpers with big-endian support
- **No duplicate allocator** — all operations delegate to Memory V2

## Module System

### KernelModule
Represents a loaded XEX with base address, size, entry point, exports, and TLS information.

### ModuleManager
Process-wide module registry supporting:
- Load/unload modules
- Get module by name or address
- Resolve exports by name or ordinal
- Enumerate all loaded modules

**Integration:** Works with XEX loader to track loaded executables and resolve imports.

## Process State

### KernelProcess

Minimal correct process environment required by retail games:

```cpp
class KernelProcess final : public KernelObject {
 public:
  ThreadManager& thread_manager();
  ModuleManager& module_manager();
  KernelMemory& memory();
  
  std::shared_ptr<KernelThread> main_thread() const;
  std::uint32_t process_id() const noexcept;
  std::uint32_t exit_code() const noexcept;
  void terminate(std::uint32_t exit_code);
};
```

**Not Implemented:** Dashboard functionality, system services (belong to XAM).

## Exception Handling

### ExceptionDispatcher

Connects CPU/Memory faults to guest exception behavior:

```cpp
class ExceptionDispatcher {
  void register_handler(ExceptionHandler handler);
  bool dispatch_memory_fault(const memory::MemoryFaultInfo& fault);
  bool dispatch_exception(const ExceptionRecord& record);
  static ExceptionRecord fault_to_exception(const memory::MemoryFaultInfo& fault);
};
```

**Fault Mapping:**
```
MemoryFaultInfo::UnmappedAddress      → ACCESS_VIOLATION (read/write)
MemoryFaultInfo::ProtectionViolation  → ACCESS_VIOLATION
MemoryFaultInfo::AlignmentFault       → ACCESS_VIOLATION (alignment)
MemoryFaultInfo::ExecuteViolation     → ACCESS_VIOLATION (execute)
```

## Export Registration

Kernel V1 exports are registered through the unified import/export system:

### Threading Exports
`ExCreateThread`, `NtCreateThread`, `ExTerminateThread`, `KeSetBasePriorityThread`, `KeSetAffinityThread`, `NtResumeThread`, `NtSuspendThread`, `KeTlsGetValue`, `KeTlsSetValue`

### Synchronization Exports
`KeSetEvent`, `KeResetEvent`, `NtCreateEvent`, `KeWaitForSingleObject`, `NtWaitForMultipleObjectsEx`, `NtCreateSemaphore`, `NtReleaseSemaphore`, `NtCreateMutant`, `NtReleaseMutant`, `NtSetTimer`, `NtCancelTimer`, `RtlInitializeCriticalSection`, `RtlEnterCriticalSection`, `RtlLeaveCriticalSection`

### Time Exports
`KeQuerySystemTime`, `KeQueryPerformanceCounter`, `KeQueryPerformanceFrequency`, `NtDelayExecution`, `KeStallExecutionProcessor`

### Memory Exports
`NtAllocateVirtualMemory`, `NtFreeVirtualMemory`, `NtProtectVirtualMemory`, `NtQueryVirtualMemory`, `MmAllocatePhysicalMemory`, `MmFreePhysicalMemory`, `MmMapIoSpace`

### Module Exports
`XexGetModuleHandle`, `XexGetProcedureAddress`, `XexGetModuleSection`

### Process Exports
`PsTerminateProcess`, `PsGetCurrentProcess`, `PsGetCurrentThread`

### Exception Exports
`RtlRaiseException`, `KiUserExceptionDispatcher`

## Integration Points

### With Memory V2
```cpp
auto address_space = std::make_shared<memory::AddressSpace>();
auto kernel_memory = std::make_shared<KernelMemory>(address_space);
auto process = std::make_shared<KernelProcess>(kernel_memory);
```

### With CPU V2
CPU execution context needs current thread for TLS, exception dispatcher for faults, and module manager for procedure lookups.

### With Runtime Session
`XenonSession` coordinates all subsystems, creating Memory V2, Kernel, Process, and connecting them.

## Testing Strategy

### Unit Tests
- Thread creation, suspend, resume, termination
- Event/semaphore/mutant/timer signaling and waiting
- Wait single/multiple object behavior
- Time conversion and sleep functions
- Memory allocation/protection/query
- Module export lookup
- Exception fault conversion

### Integration Tests
- Multi-threaded synchronization scenarios
- Thread TLS isolation
- Wait timeout accuracy
- Memory protection violations → exceptions
- Module load → export resolve → call

### Validation
Compare observable behavior against Xenia's kernel implementation.

## Performance Considerations

- **Thread Mapping:** 1:1 guest to `std::thread` initially; fiber scheduler if profiling shows need
- **Lock Contention:** Fine-grained locking; condition variables for efficient blocking
- **Memory V2:** No allocator duplication; direct delegation

## Limitations

### Not Implemented in V1
- APCs (deferred to async I/O need)
- Full timer background thread (simplified)
- Fiber scheduler (using native threads)
- Cycle-accurate scheduling
- DPCs (not needed for retail games)

### Future Enhancements
- Vectored exception handlers
- User-mode APC delivery
- Performance counter tracking per thread
- Debug/trace hooks

## Done Criteria

✅ **A recompiled title can:**
- Create threads, synchronize them, allocate memory
- Wait on objects, query time
- Invoke core xboxkrnl services through unified export mechanism

✅ **All kernel components:**
- Build successfully on Windows
- Documented in `docs/kernel/KERNEL_V1.md`
- Integrated with Memory V2, CPU V2, Runtime Session
- Provide observable Xbox 360 semantics

## File Structure

### Headers (`include/xenon/kernel/`)
- `thread.hpp` - Thread and ThreadManager
- `semaphore.hpp`, `mutant.hpp`, `timer.hpp` - Synchronization primitives
- `wait.hpp` - Wait functions
- `time.hpp` - Time services
- `memory.hpp` - Memory integration
- `module.hpp` - Module system
- `process.hpp` - Process state
- `exception.hpp` - Exception handling

### Implementation (`src/kernel/`)
- `threading/thread.cpp`
- `synchronization/` - event, semaphore, mutant, timer, wait
- `timing/time.cpp`
- `memory/memory.cpp`
- `module/module.cpp`
- `process/process.cpp`
- `exception/exception.cpp`

## References

- Xenia xboxkrnl: [https://github.com/xenia-project/xenia](https://github.com/xenia-project/xenia)
- Xbox 360 System Software: Public research
- `docs/memory/MEMORY_V2.md`
- `docs/runtime/RUNTIME_SESSION.md`
- `docs/kernel/KERNEL_IO_V1.md`

---

**Status:** ✅ Kernel V1 Complete  
**Date:** 2026-09-19  
**Components:** Object system, Threads, Synchronization, Time, Memory, Modules, Process, Exceptions
