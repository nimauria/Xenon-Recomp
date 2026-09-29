#include "xenon/xbox/xboxkrnl_ob_exports.hpp"

#include <cstdint>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/handle_table.hpp"
#include "xenon/kernel/io_types.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/kernel/thread.hpp"
#include "xenon/kernel/xbox_io.hpp"

namespace xenon::xbox {
namespace {

using xenon::core::ExportCallContext;
using xenon::kernel::Handle;
using xenon::kernel::HandleView;
using xenon::kernel::KernelIoCode;
using xenon::kernel::ObjectType;

namespace status = xenon::kernel::xbox::status;

// 0xFFFFFFFE is the real Xbox 360/Win32 NtCurrentThread() pseudo-handle
// convention (a sentinel value, never a value HandleTable actually hands
// out), matching the codebase's own kernel_process_ current-thread tracking
// (kernel::ThreadManager::current_thread()) rather than a real handle-table
// lookup.
constexpr Handle kCurrentThreadPseudoHandle = 0xFFFFFFFEu;

}  // namespace

bool ob_reference_object_by_handle_export(kernel::KernelProcess& process,
                                          ExportCallContext& context) {
  const auto handle = static_cast<Handle>(context.cpu.gpr[3]);
  const auto object_type_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  const auto out_object_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[5]);

  std::shared_ptr<kernel::KernelObject> object;
  if (handle == kCurrentThreadPseudoHandle) {
    object = process.thread_manager().current_thread();
    if (!object) {
      context.cpu.gpr[3] = status::InvalidHandle;
      return true;
    }
  } else {
    HandleView view{};
    const auto lookup_code = process.handle_table().lookup(handle, view);
    if (lookup_code != KernelIoCode::Success) {
      context.cpu.gpr[3] = status::InvalidHandle;
      return true;
    }
    object = view.object;
  }

  // Real semantics also validate object_type_ptr against the object's real
  // type (a guest-exported ExXxxObjectType global) and reject a mismatch
  // with STATUS_OBJECT_TYPE_MISMATCH. Xenon does not yet export those
  // globals to guest memory (no guest code has needed them resolved before
  // now), so a nonzero object_type_ptr cannot be checked here; every handle
  // that resolves is treated as the right type. Tightening this requires
  // exporting real guest addresses for those globals first - tracked as a
  // known gap, not silently assumed away.
  (void)object_type_ptr;

  if (out_object_ptr != 0u) {
    std::uint32_t native_address = 0u;
    if (object->type() == ObjectType::Thread) {
      native_address =
          static_cast<kernel::KernelThread&>(*object).guest_kthread_address();
    }
    // Object types without a guest-visible representation yet (Event,
    // Semaphore, Mutant, Timer, ...) get this sentinel instead of a
    // fabricated address - callers that only pass it on to
    // ObDereferenceObject (which must special-case it the same way) round-
    // trip safely; a caller that actually dereferences it will fault
    // cleanly rather than read whatever happens to be at a made-up address.
    if (!native_address) native_address = 0xDEADF00Du;
    context.memory.write32_be(out_object_ptr, native_address);
  }

  context.cpu.gpr[3] = status::Success;
  return true;
}

bool ob_dereference_object_export(kernel::KernelProcess&, ExportCallContext& context) {
  context.cpu.gpr[3] = status::Success;
  return true;
}

namespace {

std::uint32_t to_status(KernelIoCode code) {
  switch (code) {
    case KernelIoCode::Success: return status::Success;
    case KernelIoCode::InvalidHandle: return status::InvalidHandle;
    case KernelIoCode::InvalidObjectType: return status::ObjectTypeMismatch;
    case KernelIoCode::InvalidParameter: return status::InvalidParameter;
    case KernelIoCode::AccessDenied: return status::AccessDenied;
    case KernelIoCode::ProtectedHandle: return status::AccessDenied;
    default: return status::Unsuccessful;
  }
}

}  // namespace

bool nt_duplicate_object_export(kernel::KernelProcess& process, ExportCallContext& context) {
  constexpr std::uint32_t kDuplicateCloseSource = 1u;
  const auto handle = static_cast<Handle>(context.cpu.gpr[3]);
  const auto new_handle_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  const auto options_bits = static_cast<std::uint32_t>(context.cpu.gpr[5]);

  kernel::Handle new_handle{};
  KernelIoCode result_code;
  const bool is_pseudo_handle = handle == kCurrentThreadPseudoHandle;
  if (is_pseudo_handle) {
    auto object = process.thread_manager().current_thread();
    if (!object) {
      context.cpu.gpr[3] = status::InvalidHandle;
      return true;
    }
    // A pseudo-handle has no existing handle-table entry to duplicate from
    // (see kCurrentThreadPseudoHandle's comment) - materialize a real one
    // for the resolved object instead.
    result_code = process.handle_table().insert(
        std::move(object), /*granted_access=*/0u, kernel::HandleFlags::None, new_handle);
  } else {
    kernel::DuplicateHandleOptions dup_options{};
    result_code = process.handle_table().duplicate(handle, dup_options, new_handle);
  }
  if (result_code != KernelIoCode::Success) {
    context.cpu.gpr[3] = to_status(result_code);
    return true;
  }

  if (new_handle_ptr != 0u) {
    context.memory.write32_be(new_handle_ptr, new_handle);
  }
  // DUPLICATE_CLOSE_SOURCE never applies to the pseudo-handle case above -
  // there is no real source handle-table entry to close.
  if (options_bits == kDuplicateCloseSource && !is_pseudo_handle) {
    static_cast<void>(process.handle_table().close(handle));
  }
  context.cpu.gpr[3] = status::Success;
  return true;
}

}  // namespace xenon::xbox
