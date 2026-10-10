#include <cstdint>
#include <vector>

#include "xenon/core/export_registry.hpp"
#include "xenon/core/session.hpp"
#include "xenon/kernel/handle_table.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/xam/guest_enumerator.hpp"
#include "xenon/xam/xam_exports.hpp"

namespace xenon::xam {

// XamEnumerate (0x0250) - real ABI verified against xenia's
// XamEnumerate_entry(handle, flags, buffer, buffer_length, items_returned,
// overlapped): handle=r3, flags=r4, buffer=r5, buffer_length=r6,
// items_returned=r7, overlapped=r8. The previous stub read its
// "items_returned" output from gpr[5] (the buffer pointer's own register)
// instead of gpr[7], and never looked up any handle at all - a wrong-register
// bug on top of the (now-fixed) "every producer is a fake handle" gap.
//
// Walks the real kernel object a producer (xam_content_exports.cpp's
// XamContentCreateEnumerator) registered through the ordinary
// kernel::HandleTable, exactly like XStaticEnumerator<T> on real hardware -
// so this stays producer-agnostic and automatically benefits any future
// producer that backs its handle with a GuestEnumeratorObject the same way.
// Per real XStaticUntypedEnumerator::WriteItems(), the per-call item cap is
// the enumerator's own items_per_enumerate (set at creation), not
// buffer_length - this defensively also caps to buffer_length/item_size so a
// caller's under-sized buffer can never be overrun.
bool register_enum_exports(core::ExportRegistry& registry, core::XenonSession& session) {
  bool ok = true;

  core::ExportDescriptor desc{};
  desc.library = "xam";
  desc.name = "XamEnumerate";
  desc.ordinal = ordinal::XamEnumerate;
  desc.requirement = core::ExportRequirement::Required;
  desc.handler = [&session](core::ExportCallContext& ctx) -> bool {
    constexpr std::uint32_t kErrorInvalidHandle = 0x00000006u;
    constexpr std::uint32_t kErrorNoMoreFiles = 0x00000012u;

    const auto handle = static_cast<kernel::Handle>(ctx.cpu.gpr[3]);
    const auto buffer_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[5]);
    const auto buffer_length = static_cast<std::uint32_t>(ctx.cpu.gpr[6]);
    const auto items_returned_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[7]);

    auto* process = session.kernel_process();
    kernel::HandleView view{};
    if (process == nullptr ||
        process->handle_table().lookup(handle, view) != kernel::KernelIoCode::Success ||
        !view.object || view.object->type() != kernel::ObjectType::Enumerator) {
      if (items_returned_ptr != 0u) ctx.memory.write32_be(items_returned_ptr, 0u);
      ctx.cpu.gpr[3] = kErrorInvalidHandle;
      return true;
    }

    auto* enumerator = static_cast<GuestEnumeratorObject*>(view.object.get());
    if (buffer_ptr == 0u) {
      ctx.cpu.gpr[3] = result::InvalidParameter;
      return true;
    }

    std::vector<std::byte> batch;
    const auto copied = enumerator->next(batch);
    if (copied == 0u) {
      if (items_returned_ptr != 0u) ctx.memory.write32_be(items_returned_ptr, 0u);
      ctx.cpu.gpr[3] = kErrorNoMoreFiles;
      return true;
    }

    const auto item_size = enumerator->item_size();
    const auto max_bytes_from_caller_buffer =
        item_size > 0u ? buffer_length - (buffer_length % item_size) : 0u;
    const auto bytes_to_write = batch.size() > max_bytes_from_caller_buffer
                                    ? max_bytes_from_caller_buffer
                                    : static_cast<std::uint32_t>(batch.size());
    for (std::uint32_t i = 0; i < bytes_to_write; ++i) {
      ctx.memory.write8(buffer_ptr + i, static_cast<std::uint8_t>(batch[i]));
    }
    const auto items_written = item_size > 0u ? bytes_to_write / item_size : 0u;

    if (items_returned_ptr != 0u) ctx.memory.write32_be(items_returned_ptr, items_written);
    ctx.cpu.gpr[3] = result::Success;
    return true;
  };
  ok = registry.register_export(std::move(desc)) && ok;

  return ok;
}

}  // namespace xenon::xam
