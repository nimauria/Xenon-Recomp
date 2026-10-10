#include <cstdint>
#include <string>

#include "xenon/core/session.hpp"
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

namespace xenon::core {

bool XenonSession::register_kernel_object_exports() {
  // Register xboxkrnl handle-based (Nt*) synchronization exports (Phase 1/3
  // of the AC6 Runtime Readiness pass): NtCreateEvent, NtCreateSemaphore,
  // NtReleaseSemaphore, NtCreateMutant, NtReleaseMutant,
  // NtWaitForSingleObjectEx, NtWaitForMultipleObjectsEx, NtCreateTimer,
  // NtCancelTimer, NtSetTimerEx - plus the raw-guest-memory-object (Ke*)
  // variants (Event/Semaphore only - see xex_dispatcher_header.hpp for why
  // Mutant is not supported here): KeInitializeEvent, KeInitializeSemaphore,
  // KeResetEvent, KeReleaseSemaphore, KeSetEvent, KeWaitForMultipleObjects,
  // KeWaitForSingleObject. kernel_process_
  // does not exist yet at this point in a fresh session (it is created later
  // by create_guest_process(), once a title is loaded) - like io_bridge_
  // above, these lambdas capture `this` and dereference kernel_process_ at
  // CALL time, not at registration time; by the time guest code can actually
  // invoke one of these exports, create_guest_process() has already run.
  {
    using SyncHandler = bool (*)(kernel::KernelProcess&, ExportCallContext&);
    struct SyncExportBinding {
      std::uint32_t ordinal;
      const char* name;
      SyncHandler handler;
      bool partial{false};
      const char* partial_note{};
    };
    static constexpr SyncExportBinding kSyncBindings[] = {
        {0x0D1u, "NtCreateEvent", &xbox::nt_create_event_export},
        {0x0D5u, "NtCreateSemaphore", &xbox::nt_create_semaphore_export},
        {0x0F3u, "NtReleaseSemaphore", &xbox::nt_release_semaphore_export},
        {0x0D4u, "NtCreateMutant", &xbox::nt_create_mutant_export},
        {0x0F2u, "NtReleaseMutant", &xbox::nt_release_mutant_export},
        {0x0FDu, "NtWaitForSingleObjectEx", &xbox::nt_wait_for_single_object_ex_export},
        {0x0FEu, "NtWaitForMultipleObjectsEx", &xbox::nt_wait_for_multiple_objects_ex_export},
        {0x0D7u, "NtCreateTimer", &xbox::nt_create_timer_export},
        {0x0CDu, "NtCancelTimer", &xbox::nt_cancel_timer_export},
        {0x0FAu, "NtSetTimerEx", &xbox::nt_set_timer_ex_export},
        {0x070u, "KeInitializeEvent", &xbox::ke_initialize_event_export},
        {0x074u, "KeInitializeSemaphore", &xbox::ke_initialize_semaphore_export},
        {0x08Fu, "KeResetEvent", &xbox::ke_reset_event_export},
        {0x088u, "KeReleaseSemaphore", &xbox::ke_release_semaphore_export},
        {0x09Du, "KeSetEvent", &xbox::ke_set_event_export},
        {0x0AFu, "KeWaitForMultipleObjects", &xbox::ke_wait_for_multiple_objects_export},
        {0x0B0u, "KeWaitForSingleObject", &xbox::ke_wait_for_single_object_export},
        {0x0CCu, "NtAllocateVirtualMemory", &xbox::nt_allocate_virtual_memory_export},
        {0x0DCu, "NtFreeVirtualMemory", &xbox::nt_free_virtual_memory_export},
        {0x009u, "ExAllocatePool", &xbox::ex_allocate_pool_export},
        {0x011u, "ExInitializeReadWriteLock", &xbox::ex_initialize_read_write_lock_export},
        {0x007u, "ExAcquireReadWriteLockExclusive",
         &xbox::ex_acquire_read_write_lock_exclusive_export},
        {0x008u, "ExAcquireReadWriteLockShared", &xbox::ex_acquire_read_write_lock_shared_export},
        {0x2DDu, "ExTryToAcquireReadWriteLockExclusive",
         &xbox::ex_try_to_acquire_read_write_lock_exclusive_export},
        {0x2DEu, "ExTryToAcquireReadWriteLockShared",
         &xbox::ex_try_to_acquire_read_write_lock_shared_export},
        {0x016u, "ExReleaseReadWriteLock", &xbox::ex_release_read_write_lock_export},
        {0x07Fu, "KePulseEvent", &xbox::ke_pulse_event_export},
        {0x0E6u, "NtQueryEvent", &xbox::nt_query_event_export},
        {0x0A9u, "KeSuspendThread", &xbox::ke_suspend_thread_export},
        {0x0FCu, "NtSuspendThread", &xbox::nt_suspend_thread_export},
        {0x09Cu, "KeSetDisableBoostThread", &xbox::ke_set_disable_boost_thread_export},
        {0x020u, "FscGetCacheElementCount", &xbox::fsc_get_cache_element_count_export},
        {0x021u, "FscSetCacheElementCount", &xbox::fsc_set_cache_element_count_export},
        {0x00Au, "ExAllocatePoolWithTag", &xbox::ex_allocate_pool_with_tag_export},
        {0x00Bu, "ExAllocatePoolTypeWithTag", &xbox::ex_allocate_pool_type_with_tag_export},
        {0x00Fu, "ExFreePool", &xbox::ex_free_pool_export},
        {0x013u, "ExQueryPoolBlockSize", &xbox::ex_query_pool_block_size_export},
        {0x0EEu, "NtQueryVirtualMemory", &xbox::nt_query_virtual_memory_export},
        {0x0E1u, "NtProtectVirtualMemory", &xbox::nt_protect_virtual_memory_export},
        {0x0B9u, "MmAllocatePhysicalMemory", &xbox::mm_allocate_physical_memory_export},
        {0x0C2u, "MmMapIoSpace", &xbox::mm_map_io_space_export},
        {0x0BFu, "MmIsAddressValid", &xbox::mm_is_address_valid_export},
        {0x0BBu, "MmCreateKernelStack", &xbox::mm_create_kernel_stack_export},
        {0x0BCu, "MmDeleteKernelStack", &xbox::mm_delete_kernel_stack_export},
        {0x2A4u, "KeGetImagePageTableEntry", &xbox::ke_get_image_page_table_entry_export},
        {0x28Au, "NtAllocateEncryptedMemory", &xbox::nt_allocate_encrypted_memory_export},
        {0x28Bu, "NtFreeEncryptedMemory", &xbox::nt_free_encrypted_memory_export},
        {0x0BAu, "MmAllocatePhysicalMemoryEx", &xbox::mm_allocate_physical_memory_ex_export},
        {0x0BDu, "MmFreePhysicalMemory", &xbox::mm_free_physical_memory_export},
        {0x0BEu, "MmGetPhysicalAddress", &xbox::mm_get_physical_address_export},
        {0x0C4u, "MmQueryAddressProtect", &xbox::mm_query_address_protect_export},
        {0x0C5u, "MmQueryAllocationSize", &xbox::mm_query_allocation_size_export},
        {0x0C6u, "MmQueryStatistics", &xbox::mm_query_statistics_export, true,
         "returns real total/available/used/image/virtual page counts, but leaves "
         "pool/stack/heap/page-table/cache subdivisions zero because Memory V2 "
         "does not attribute allocations to those hardware-specific buckets"},
        {0x0C7u, "MmSetAddressProtect", &xbox::mm_set_address_protect_export},
        {0x110u, "ObReferenceObjectByHandle", &xbox::ob_reference_object_by_handle_export},
        {0x105u, "ObDereferenceObject", &xbox::ob_dereference_object_export},
        {0x0DAu, "NtDuplicateObject", &xbox::nt_duplicate_object_export},
        {0x0CEu, "NtClearEvent", &xbox::nt_clear_event_export},
        {0x0F6u, "NtSetEvent", &xbox::nt_set_event_export},
        {0x0E2u, "NtPulseEvent", &xbox::nt_pulse_event_export},
        {0x019u, "ExTerminateThread", &xbox::ex_terminate_thread_export},
        {0x0F5u, "NtResumeThread", &xbox::nt_resume_thread_export},
        {0x099u, "KeSetBasePriorityThread", &xbox::ke_set_base_priority_thread_export},
        {0x081u, "KeQueryBasePriorityThread", &xbox::ke_query_base_priority_thread_export},
        {0x097u, "KeSetAffinityThread", &xbox::ke_set_affinity_thread_export},
        {0x092u, "KeResumeThread", &xbox::ke_resume_thread_export},
        {0x066u, "KeGetCurrentProcessType", &xbox::ke_get_current_process_type_export},
        {0x09Au, "KeSetCurrentProcessType", &xbox::ke_set_current_process_type_export},
        {0x125u, "RtlEnterCriticalSection", &xbox::rtl_enter_critical_section_export},
        {0x12Eu, "RtlInitializeCriticalSection", &xbox::rtl_initialize_critical_section_export},
        {0x12Fu, "RtlInitializeCriticalSectionAndSpinCount",
         &xbox::rtl_initialize_critical_section_and_spin_count_export},
        {0x130u, "RtlLeaveCriticalSection", &xbox::rtl_leave_critical_section_export},
        {0x141u, "RtlTryEnterCriticalSection", &xbox::rtl_try_enter_critical_section_export},
        {0x152u, "KeTlsAlloc", &xbox::ke_tls_alloc_export},
        {0x153u, "KeTlsFree", &xbox::ke_tls_free_export},
        {0x154u, "KeTlsGetValue", &xbox::ke_tls_get_value_export},
        {0x155u, "KeTlsSetValue", &xbox::ke_tls_set_value_export},
        // Vd* Xenos GPU control-plane exports - hand the guest's GPU command
        // ring buffer, front buffer and vblank interrupt callback over to
        // KernelProcess's Gpu*State (see xboxkrnl_video_exports.cpp/
        // process.hpp), the write side of the contract a separate,
        // already-landed XenonSession GPU pump thread workstream reads from.
        {0x1B1u, "VdCallGraphicsNotificationRoutines",
         &xbox::vd_call_graphics_notification_routines_export},
        {0x1B6u, "VdEnableRingBufferRPtrWriteBack",
         &xbox::vd_enable_ring_buffer_rptr_write_back_export},
        {0x1BAu, "VdGetCurrentDisplayInformation",
         &xbox::vd_get_current_display_information_export},
        {0x1BDu, "VdGetSystemCommandBuffer", &xbox::vd_get_system_command_buffer_export},
        {0x1C2u, "VdInitializeEngines", &xbox::vd_initialize_engines_export},
        {0x1C3u, "VdInitializeRingBuffer", &xbox::vd_initialize_ring_buffer_export},
        {0x1C6u, "VdIsHSIOTrainingSucceeded", &xbox::vd_is_hsio_training_succeeded_export},
        {0x1CAu, "VdQueryVideoMode", &xbox::vd_query_video_mode_export},
        {0x1D5u, "VdSetGraphicsInterruptCallback",
         &xbox::vd_set_graphics_interrupt_callback_export},
        {0x1D9u, "VdSetSystemCommandBufferGpuIdentifierAddress",
         &xbox::vd_set_system_command_buffer_gpu_identifier_address_export},
        {0x1DCu, "VdShutdownEngines", &xbox::vd_shutdown_engines_export},
        {0x25Bu, "VdSwap", &xbox::vd_swap_export},
        {0x0CFu, "NtClose", &xbox::nt_close_export},
        {0x0FBu, "NtSignalAndWaitForSingleObjectEx",
         &xbox::nt_signal_and_wait_for_single_object_ex_export},
        {0x1B4u, "VdEnableDisableClockGating", &xbox::vd_enable_disable_clock_gating_export},
        {0x1B9u, "VdGetCurrentDisplayGamma", &xbox::vd_get_current_display_gamma_export},
        {0x1C9u, "VdQueryVideoFlags", &xbox::vd_query_video_flags_export},
        {0x1D3u, "VdSetDisplayMode", &xbox::vd_set_display_mode_export},
        {0x1C5u, "VdInitializeScalerCommandBuffer",
         &xbox::vd_initialize_scaler_command_buffer_export},
        {0x1C7u, "VdPersistDisplay", &xbox::vd_persist_display_export},
        {0x269u, "VdRetrainEDRAM", &xbox::vd_retrain_edram_export},
        {0x26Au, "VdRetrainEDRAMWorker", &xbox::vd_retrain_edram_worker_export},
        // Low-level block-device IOCTL exports - see
        // xboxkrnl_device_io_exports.cpp for the real behavior each
        // implements; none need KernelProcess state, but match this table's
        // shared SyncHandler signature.
        {0xD9u, "NtDeviceIoControlFile", &xbox::nt_device_io_control_file_export, true,
         "only the two real IOCTLs AC6's XMountUtilityDrive cache-mounting "
         "path issues (DISK_GET_DRIVE_GEOMETRY, DISK_GET_PARTITION_INFO) are "
         "handled; any other IOCTL reports InvalidParameter"},
        {0x3Bu, "IoDismountVolume", &xbox::io_dismount_volume_export, true,
         "no dynamic per-volume mount-table entry exists to actually "
         "dismount - matches rexglue-sdk's identical precedent"},
        {0x3Cu, "IoDismountVolumeByFileHandle", &xbox::io_dismount_volume_export, true,
         "no dynamic per-volume mount-table entry exists to actually "
         "dismount - matches rexglue-sdk's identical precedent"},
        {0x259u, "StfsCreateDevice", &xbox::stfs_device_export, true,
         "the low-level STFS NT device path is not backed by a real device "
         "object - modern content access goes through XamContent*/"
         "ContentManager instead"},
        {0x25Au, "StfsControlDevice", &xbox::stfs_device_export, true,
         "the low-level STFS NT device path is not backed by a real device "
         "object - modern content access goes through XamContent*/"
         "ContentManager instead"},
    };
    for (const auto& binding : kSyncBindings) {
      core::ExportDescriptor descriptor{};
      descriptor.library = "xboxkrnl.exe";
      descriptor.name = binding.name;
      descriptor.ordinal = binding.ordinal;
      descriptor.requirement = ExportRequirement::Required;
      descriptor.partial = binding.partial;
      if (binding.partial_note) descriptor.partial_note = binding.partial_note;
      descriptor.handler = [this, fn = binding.handler](ExportCallContext& ctx) -> bool {
        if (!kernel_process_) return false;
        return fn(*kernel_process_, ctx);
      };
      if (!export_registry_.register_export(std::move(descriptor))) {
        set_error(std::string("Failed to register xboxkrnl sync export: ") + binding.name);
        return false;
      }
    }
  }
  return true;
}

}  // namespace xenon::core
