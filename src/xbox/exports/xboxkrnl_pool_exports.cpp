#include "xenon/xbox/xboxkrnl_pool_exports.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

#include "xenon/core/export_registry.hpp"
#include "xenon/kernel/process.hpp"
#include "xenon/logging/logger.hpp"

namespace xenon::xbox {
namespace {

using core::ExportCallContext;

// The tag ExAllocatePool uses when the caller supplies none: 'None' as the
// kernel's own little-endian-in-memory tag constant (0x656E6F4E).
constexpr std::uint32_t kDefaultTag = 0x656E6F4Eu;

bool allocate(kernel::KernelProcess& process, ExportCallContext& context, std::uint32_t tag) {
  const auto size = static_cast<std::uint32_t>(context.cpu.gpr[3]);
  context.cpu.gpr[3] = process.pool().allocate(size, tag);
  return true;
}

}  // namespace

bool ex_allocate_pool_export(kernel::KernelProcess& process, ExportCallContext& context) {
  return allocate(process, context, kDefaultTag);
}

bool ex_allocate_pool_with_tag_export(kernel::KernelProcess& process,
                                      ExportCallContext& context) {
  return allocate(process, context, static_cast<std::uint32_t>(context.cpu.gpr[4]));
}

bool ex_allocate_pool_type_with_tag_export(kernel::KernelProcess& process,
                                           ExportCallContext& context) {
  return allocate(process, context, static_cast<std::uint32_t>(context.cpu.gpr[4]));
}

// A pointer the pool never issued, or has already freed, is a title bug that
// real hardware answers with a BAD_POOL_CALLER bugcheck. Xenon rejects the free
// without touching any arena and reports it at Error level so the bug is
// diagnosable; it does not take the whole title down over it.
bool ex_free_pool_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto address = static_cast<std::uint32_t>(context.cpu.gpr[3]);
  if (!process.pool().free(address)) {
    logging::Logger::instance().log_if_enabled(logging::Level::Error, "pool", [&] {
      return "ExFreePool of an address the pool did not issue or already freed: 0x" +
             [&] {
               char text[16];
               std::snprintf(text, sizeof(text), "%08X", address);
               return std::string(text);
             }();
    });
  }
  return true;
}

bool ex_query_pool_block_size_export(kernel::KernelProcess& process, ExportCallContext& context) {
  const auto address = static_cast<std::uint32_t>(context.cpu.gpr[3]);
  const auto quota_ptr = static_cast<cpu::GuestAddress>(context.cpu.gpr[4]);
  if (quota_ptr != 0u) context.memory.write8(quota_ptr, 0u);  // no quota is ever charged
  context.cpu.gpr[3] = process.pool().usable_size(address);
  return true;
}

}  // namespace xenon::xbox
