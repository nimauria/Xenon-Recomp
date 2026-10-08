#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>

#include "xenon/core/session.hpp"
#include "xenon/kernel/xbox_io.hpp"
#include "xenon/xam/xam_exports.hpp"
#include "xenon/xbox/xboxkrnl_rtl_exports.hpp"
#include "xenon/xbox/xboxkrnl_rtl_string_exports.hpp"
#include "xenon/xbox/xboxkrnl_ke_irql_exports.hpp"
#include "xenon/xbox/xboxkrnl_misc_exports.hpp"
#include "xenon/xbox/xboxkrnl_string_exports.hpp"
#include "xenon/xbox/xboxkrnl_ke_sync_exports.hpp"
#include "xenon/xbox/xboxkrnl_sync_exports.hpp"
#include "xenon/xbox/xboxkrnl_threading_exports.hpp"
#include "xenon/xbox/xboxkrnl_memory_exports.hpp"
#include "xenon/xbox/xboxkrnl_pool_exports.hpp"
#include "xenon/xbox/xboxkrnl_ob_exports.hpp"
#include "xenon/xbox/xboxkrnl_process_exports.hpp"
#include "xenon/xbox/xboxkrnl_rtl_critical_section_exports.hpp"
#include "xenon/xbox/xboxkrnl_time_exports.hpp"
#include "xenon/xbox/xboxkrnl_tls_exports.hpp"
#include "xenon/xbox/xboxkrnl_device_io_exports.hpp"
#include "xenon/xbox/xboxkrnl_video_exports.hpp"
#include "xenon/xbox/xboxkrnl_xex_module_exports.hpp"

#if defined(XENON_HAS_AUDIO)
#include "xenon/audio/exports.hpp"
#include "xenon/audio/system.hpp"
#endif

namespace xenon::core {

// Registration order is part of the session contract: system modules first,
// then the per-process kernel object exports, session-owned exports, and the
// XAM/audio/input/I/O bridges that dispatch into session subsystems.
bool XenonSession::init_exports() {
  // Register core exports
  export_registry_.clear();
  if (memory_) {
    module_registry_ = std::make_unique<xbox::GuestModuleRegistry>(*memory_, export_registry_);
  }
  
  if (!register_process_free_kernel_exports()) return false;
  if (!register_kernel_object_exports()) return false;
  if (!register_session_bound_kernel_exports()) return false;
  if (!register_subsystem_bridge_exports()) return false;

  if (!init_kernel_variable_exports()) {
    set_error("Failed to initialize xboxkrnl variable exports");
    return false;
  }

  return true;
}

// xboxkrnl exports implemented entirely by free functions with no
// KernelProcess dependency.
bool XenonSession::register_process_free_kernel_exports() {
  // Register xboxkrnl RTL exports (RtlImageXexHeaderField, etc.)
  if (!xbox::register_xboxkrnl_rtl_exports(export_registry_)) {
    set_error("Failed to register xboxkrnl RTL exports");
    return false;
  }

  // Register the Rtl ANSI_STRING/UNICODE_STRING/character family. Conversions
  // that allocate their destination (and the Free*String exports) reach the
  // process's kernel pool through hooks that dereference kernel_process_ at CALL
  // time - the process does not exist yet when exports are registered.
  {
    xbox::RtlPoolHooks hooks;
    hooks.allocate = [this](std::uint32_t size) -> std::uint32_t {
      return kernel_process_ ? kernel_process_->pool().allocate(size, 0x656E6F4Eu) : 0u;
    };
    hooks.free = [this](std::uint32_t address) -> bool {
      return kernel_process_ && kernel_process_->pool().free(address);
    };
    if (!xbox::register_xboxkrnl_rtl_string_exports(export_registry_, std::move(hooks))) {
      set_error("Failed to register xboxkrnl Rtl string exports");
      return false;
    }
  }

  // Register xboxkrnl guest timebase/timing exports (Phase 1 of the AC6
  // Runtime Readiness pass): KeQueryPerformanceFrequency, KeQuerySystemTime,
  // KeDelayExecutionThread, KeStallExecutionProcessor.
  if (!xbox::register_xboxkrnl_time_exports(export_registry_)) {
    set_error("Failed to register xboxkrnl time exports");
    return false;
  }

  // Register xboxkrnl guest memory-management exports: KeFlushUserModeTb.
  if (!xbox::register_xboxkrnl_memory_exports(export_registry_)) {
    set_error("Failed to register xboxkrnl memory exports");
    return false;
  }

  // Register xboxkrnl IRQL/critical-region/spin-lock exports
  // (KeEnterCriticalRegion, KfAcquireSpinLock, etc.) - no KernelProcess
  // dependency, so these can register directly like RTL/time/memory above.
  if (!xbox::register_xboxkrnl_ke_irql_exports(export_registry_)) {
    set_error("Failed to register xboxkrnl Ke IRQL/spin-lock exports");
    return false;
  }

  // Register xboxkrnl misc exports (XeCryptSha, ExGetXConfigSetting,
  // ExRegisterTitleTerminateNotification) - no KernelProcess dependency.
  if (!xbox::register_xboxkrnl_misc_exports(export_registry_)) {
    set_error("Failed to register xboxkrnl misc exports");
    return false;
  }

  // Register the xboxkrnl formatted-output family (sprintf/_snprintf/vsprintf/...
  // and their wide variants) and DbgPrint - native host code, no KernelProcess.
  if (!xbox::register_xboxkrnl_string_exports(export_registry_)) {
    set_error("Failed to register xboxkrnl string/format exports");
    return false;
  }

  // Register the process-free threading exports (interlocked SLists,
  // NtYieldExecution, KfRaiseIrql, KeEnableFpuExceptions).
  if (!xbox::register_xboxkrnl_threading_exports(export_registry_)) {
    set_error("Failed to register xboxkrnl threading exports");
    return false;
  }

  // Register the kernel debug exports (DbgBreakPoint*, DbgPrompt,
  // KiApcNormalRoutineNop) and KeBugCheck/KeBugCheckEx. A bugcheck is fatal on
  // real hardware: fail the session with the stop code and request the same
  // cooperative stop HalReturnToFirmware uses.
  if (!xbox::register_xboxkrnl_debug_exports(export_registry_)) {
    set_error("Failed to register xboxkrnl debug exports");
    return false;
  }
  if (!xbox::register_xboxkrnl_bugcheck_exports(
          export_registry_, [this](const xbox::BugCheckInfo& info) {
            if (config_.enable_logging) {
              std::scoped_lock console_log_lock(console_log_mutex());
              std::cout << "[XenonSession] " << info.description << std::endl;
            }
            set_error(info.description);
            stop_requested_.store(true, std::memory_order_relaxed);
          })) {
    set_error("Failed to register xboxkrnl bugcheck exports");
    return false;
  }
  return true;
}

// Exports whose implementation needs private XenonSession state (the loaded
// XEX, module registry, filesystem, stop flag or guest-thread creation).
bool XenonSession::register_session_bound_kernel_exports() {
  // Register XexCheckExecutablePrivilege (ordinal 0x194 / 404 - AC6's boot
  // path calls this immediately after its first RtlEnterCriticalSection/
  // RtlLeaveCriticalSection pair). loaded_xex_ does not exist yet at this
  // point in a fresh session either - same lazy-dereference-at-call-time
  // pattern as kernel_process_ above.
  {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = "XexCheckExecutablePrivilege";
    descriptor.ordinal = 0x194u;
    descriptor.requirement = ExportRequirement::Required;
    descriptor.handler = [this](ExportCallContext& ctx) -> bool {
      if (!loaded_xex_) return false;
      return xbox::xex_check_executable_privilege_export(loaded_xex_->image, ctx);
    };
    if (!export_registry_.register_export(std::move(descriptor))) {
      set_error("Failed to register xboxkrnl XexCheckExecutablePrivilege export");
      return false;
    }
  }

  // Register XexGetModuleHandle (0x195) / XexGetProcedureAddress (0x197). They
  // resolve through module_registry_, which is (re)created above with this
  // registry, so a lookup only ever sees the exports registered here.
  {
    core::ExportDescriptor handle_descriptor{};
    handle_descriptor.library = "xboxkrnl.exe";
    handle_descriptor.name = "XexGetModuleHandle";
    handle_descriptor.ordinal = 0x195u;
    handle_descriptor.requirement = ExportRequirement::Required;
    handle_descriptor.handler = [this](ExportCallContext& ctx) -> bool {
      if (!module_registry_) return false;
      return xbox::xex_get_module_handle_export(*module_registry_, ctx);
    };
    if (!export_registry_.register_export(std::move(handle_descriptor))) {
      set_error("Failed to register xboxkrnl XexGetModuleHandle export");
      return false;
    }
    core::ExportDescriptor proc_descriptor{};
    proc_descriptor.library = "xboxkrnl.exe";
    proc_descriptor.name = "XexGetProcedureAddress";
    proc_descriptor.ordinal = 0x197u;
    proc_descriptor.requirement = ExportRequirement::Required;
    proc_descriptor.handler = [this](ExportCallContext& ctx) -> bool {
      if (!module_registry_) return false;
      return xbox::xex_get_procedure_address_export(*module_registry_, ctx);
    };
    if (!export_registry_.register_export(std::move(proc_descriptor))) {
      set_error("Failed to register xboxkrnl XexGetProcedureAddress export");
      return false;
    }
  }

  // Register HalReturnToFirmware (ordinal 0x28 / 40). Real hardware: void
  // HalReturnToFirmware(FIRMWARE_REENTRY routine) - routine must be 1
  // (HalRebootRoutine); control never returns to the caller (the console
  // reboots/halts). rexglue-sdk's own reference implementation
  // (HalReturnToFirmware_entry) simply calls the host's exit(0) - too abrupt
  // for Xenon, which owns a status.json/log.txt writer contract
  // (docs/runtime/RUNTIME_HOST.md) a hard process exit would skip entirely.
  // Xenon instead requests the same cooperative stop a launcher's
  // stop.signal produces (sets stop_requested_ - see "Stopping a session" in
  // RUNTIME_HOST.md), so the guest execution thread unwinds through its own
  // normal shutdown path and status.json still reaches a real terminal
  // state, rather than the process vanishing mid-write.
  {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = "HalReturnToFirmware";
    descriptor.ordinal = 0x28u;
    descriptor.requirement = ExportRequirement::Required;
    descriptor.handler = [this](ExportCallContext& ctx) -> bool {
      if (config_.enable_logging) {
        std::scoped_lock console_log_lock(console_log_mutex());
        std::cout << "[XenonSession] Guest requested HalReturnToFirmware(routine="
                  << ctx.cpu.gpr[3] << ") - requesting cooperative session stop" << std::endl;
      }
      stop_requested_.store(true, std::memory_order_relaxed);
      return true;
    };
    if (!export_registry_.register_export(std::move(descriptor))) {
      set_error("Failed to register xboxkrnl HalReturnToFirmware export");
      return false;
    }
  }

  // Register ObCreateSymbolicLink/ObDeleteSymbolicLink (ordinals 0x103/259,
  // 0x104/260). Real hardware: NTSTATUS ObCreateSymbolicLink(PANSI_STRING
  // path, PANSI_STRING target) registers a path alias (e.g. a title mapping
  // its own logical device name to a real Xbox path) in the kernel object
  // namespace; ObDeleteSymbolicLink(PANSI_STRING path) removes one. Routed
  // to filesystem_'s real symbolic-link table
  // (filesystem::VirtualFileSystem::register_symbolic_link/
  // unregister_symbolic_link - the same mechanism the launcher/content
  // system uses), not a fabricated no-op - a later NtCreateFile-style guest
  // path lookup through that alias resolves for real. filesystem_ does not
  // exist yet at this point in a fresh session (same lazy-dereference-at-
  // call-time pattern as kernel_process_/loaded_xex_ above).
  {
    // ANSI_STRING layout (matches RtlInitAnsiString in xboxkrnl_rtl_exports.cpp):
    // +0x0 Length (u16), +0x2 MaximumLength (u16), +0x4 Buffer (u32 guest ptr).
    auto read_ansi_string = [](cpu::MemoryPort& memory, cpu::GuestAddress ptr) -> std::string {
      if (ptr == 0u) return {};
      const auto length = memory.read16_be(ptr + 0u);
      const auto buffer = memory.read32_be(ptr + 4u);
      if (buffer == 0u || length == 0u) return {};
      std::string result;
      result.reserve(length);
      for (std::uint16_t i = 0; i < length; ++i) {
        result.push_back(static_cast<char>(memory.read8(buffer + i)));
      }
      return result;
    };

    core::ExportDescriptor create_descriptor{};
    create_descriptor.library = "xboxkrnl.exe";
    create_descriptor.name = "ObCreateSymbolicLink";
    create_descriptor.ordinal = 0x103u;
    create_descriptor.requirement = ExportRequirement::Required;
    create_descriptor.handler = [this, read_ansi_string](ExportCallContext& ctx) -> bool {
      if (!filesystem_) {
        ctx.cpu.gpr[3] = kernel::xbox::status::Unsuccessful;
        return true;
      }
      auto path = read_ansi_string(ctx.memory, static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]));
      const auto target =
          read_ansi_string(ctx.memory, static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]));
      constexpr std::string_view kNtObjectPrefix = "\\??\\";
      if (path.rfind(kNtObjectPrefix, 0) == 0) path = path.substr(kNtObjectPrefix.size());

      const auto error = filesystem_->register_symbolic_link(path, target);
      ctx.cpu.gpr[3] = (error == filesystem::FsError::None)
                            ? kernel::xbox::status::Success
                            : kernel::xbox::status::Unsuccessful;
      return true;
    };
    if (!export_registry_.register_export(std::move(create_descriptor))) {
      set_error("Failed to register xboxkrnl ObCreateSymbolicLink export");
      return false;
    }

    core::ExportDescriptor delete_descriptor{};
    delete_descriptor.library = "xboxkrnl.exe";
    delete_descriptor.name = "ObDeleteSymbolicLink";
    delete_descriptor.ordinal = 0x104u;
    delete_descriptor.requirement = ExportRequirement::Required;
    delete_descriptor.handler = [this, read_ansi_string](ExportCallContext& ctx) -> bool {
      if (!filesystem_) {
        ctx.cpu.gpr[3] = kernel::xbox::status::Unsuccessful;
        return true;
      }
      auto path = read_ansi_string(ctx.memory, static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]));
      constexpr std::string_view kNtObjectPrefix = "\\??\\";
      if (path.rfind(kNtObjectPrefix, 0) == 0) path = path.substr(kNtObjectPrefix.size());

      const auto error = filesystem_->unregister_symbolic_link(path);
      ctx.cpu.gpr[3] = (error == filesystem::FsError::None)
                            ? kernel::xbox::status::Success
                            : kernel::xbox::status::Unsuccessful;
      return true;
    };
    if (!export_registry_.register_export(std::move(delete_descriptor))) {
      set_error("Failed to register xboxkrnl ObDeleteSymbolicLink export");
      return false;
    }
  }

  // Register ExCreateThread (Phase 1/2 of the AC6 Runtime Readiness pass).
  // Ordinal 0x0D verified against the xenia-project/xenia xboxkrnl export
  // table (xboxkrnl_table.inc). A private XenonSession method rather than a
  // free function like the sync exports above: spawning a real
  // guest-executing thread needs compiled_registry_binder_/dynamic_fallback_/
  // memory_/the XEX's TLS template, not just kernel_process_.
  {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = "ExCreateThread";
    descriptor.ordinal = 0x0Du;
    descriptor.requirement = ExportRequirement::Required;
    descriptor.handler = [this](ExportCallContext& ctx) -> bool {
      return export_ex_create_thread(ctx);
    };
    if (!export_registry_.register_export(std::move(descriptor))) {
      set_error("Failed to register xboxkrnl ExCreateThread export");
      return false;
    }
  }
  return true;
}

// XAM, XamTaskSchedule, audio, XamInput and xboxkrnl file I/O, each bridged
// into the session subsystem that owns its state.
bool XenonSession::register_subsystem_bridge_exports() {
  // Register XAM exports
  if (xam_ && !xam_->register_exports(export_registry_, *this)) {
    set_error("Failed to register XAM exports");
    return false;
  }

  // Register XamTaskSchedule (xam.xex ordinal 0x01AF). Like ExCreateThread
  // above, spawning a real guest-executing thread needs
  // compiled_registry_binder_/dynamic_fallback_/memory_/the XEX's TLS
  // template - private XenonSession state a free xam/ export file cannot
  // reach - so this is registered inline here, not in xam_session.cpp.
  {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xam";
    descriptor.name = "XamTaskSchedule";
    descriptor.ordinal = xam::ordinal::XamTaskSchedule;
    descriptor.requirement = ExportRequirement::Required;
    descriptor.handler = [this](ExportCallContext& ctx) -> bool {
      return export_xam_task_schedule(ctx);
    };
    if (!export_registry_.register_export(std::move(descriptor))) {
      set_error("Failed to register xam XamTaskSchedule export");
      return false;
    }
  }

#if defined(XENON_HAS_AUDIO)
  if (config_.enable_audio && audio_ &&
      !audio::register_xbox_audio_exports(export_registry_, *audio_)) {
    set_error("Failed to register Xbox audio exports");
    return false;
  }
#endif

  // Guest XamInput* calls go through the same canonical export_registry_ as
  // XAM/Audio above (see input_bridge_'s comment in session.hpp) rather than
  // a separate dispatcher, so they reach the InputSystem owned by this
  // session regardless of which module/ordinal table a title imports them
  // from.
  if (input_bridge_) {
    for (const auto& desc : input::xam::guest::exports()) {
      core::ExportDescriptor export_desc{};
      export_desc.library = "xam";
      export_desc.name = std::string(desc.name);
      export_desc.ordinal = desc.ordinal;
      export_desc.requirement = ExportRequirement::Required;
      export_desc.handler = [this, ordinal = desc.ordinal](ExportCallContext& ctx) {
        return input_bridge_->dispatch(ordinal, ctx.cpu, ctx.memory);
      };
      if (!export_registry_.register_export(std::move(export_desc))) {
        set_error("Failed to register XamInput export");
        return false;
      }
    }
  }

  // Bridge xboxkrnl file I/O (NtCreateFile, NtReadFile, ...) into the same
  // canonical export_registry_. xbox_imports_ already carries the real
  // ordinal/thunk table (register_xboxkrnl_io_imports(), init_kernel()); each
  // thunk operates on io_bridge_, which wraps this session's own kernel_io_ -
  // so this is the active session's real filesystem state, not a global.
  if (io_bridge_) {
    for (const auto& desc : xbox_imports_.enumerate("xboxkrnl")) {
      core::ExportDescriptor export_desc{};
      export_desc.library = desc.module;
      export_desc.name = desc.name;
      export_desc.ordinal = desc.ordinal;
      export_desc.requirement = ExportRequirement::Required;
      export_desc.handler = [this, ordinal = desc.ordinal](ExportCallContext& ctx) {
        xbox::ImportCallContext import_ctx{ctx.cpu, *memory_, *io_bridge_};
        return xbox_imports_.invoke("xboxkrnl", ordinal, import_ctx);
      };
      if (!export_registry_.register_export(std::move(export_desc))) {
        set_error("Failed to register xboxkrnl I/O export");
        return false;
      }
    }
  }
  return true;
}

}  // namespace xenon::core
