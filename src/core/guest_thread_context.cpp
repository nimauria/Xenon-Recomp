#include "xenon/core/guest_thread_context.hpp"

#include <algorithm>
#include <string>

namespace xenon::core {

bool setup_guest_thread_tls_context(memory::AddressSpace& memory,
                                    const std::optional<xbox::XexTls>& tls_info,
                                    memory::GuestAddress stack_base,
                                    std::uint32_t stack_size,
                                    GuestThreadTlsContext& out_context,
                                    std::string* error) {
  out_context = {};

  memory::GuestAddress kpcr_address{};
  if (!memory.allocate(GuestKpcrLayout::kSize, 16, memory::kReadWrite,
                       /*top_down=*/false, kpcr_address)) {
    if (error) *error = "failed to allocate guest KPCR block";
    return false;
  }
  memory.zero(kpcr_address, GuestKpcrLayout::kSize);

  memory::GuestAddress tls_address{};
  std::uint32_t tls_size = 0;
  if (tls_info && tls_info->data_size > 0) {
    tls_size = tls_info->data_size;
    if (!memory.allocate(tls_size, 16, memory::kReadWrite, /*top_down=*/false,
                         tls_address)) {
      if (error) *error = "failed to allocate guest TLS block";
      static_cast<void>(memory.release(kpcr_address));
      return false;
    }
    memory.zero(tls_address, tls_size);
    if (tls_info->raw_data_size > 0) {
      // raw_data_start is already an absolute guest virtual address (the
      // XEX/PE TLS directory format stores a VA here, not an image-relative
      // RVA), and the effective image is already mapped by the time
      // XenonSession::create_guest_process() runs this, so a direct guest
      // copy is correct.
      const auto copy_size = std::min(tls_info->raw_data_size, tls_size);
      memory.copy(tls_address, tls_info->raw_data_start, copy_size);
    }
  }

  // The stack grows downward from stack_base; stack_base is the highest
  // address of the allocation (see create_guest_process()), matching
  // GuestKpcrLayout::kStackBaseOffset's "high address" semantics.
  memory.write32_be(kpcr_address + GuestKpcrLayout::kSelfOffset, kpcr_address);
  memory.write32_be(kpcr_address + GuestKpcrLayout::kStackBaseOffset, stack_base);
  memory.write32_be(kpcr_address + GuestKpcrLayout::kStackLimitOffset,
                    stack_base - stack_size);
  if (tls_address) {
    memory.write32_be(kpcr_address + GuestKpcrLayout::kTlsPtrOffset, tls_address);
  }

  // A nonzero index_address is a compiler-emitted "TLS index" global (the
  // Win32-PE-TLS-directory convention): a single process-wide constant every
  // thread's compiled code reads to know which TLS block layout to use.
  // Xenon only ever instantiates one static-TLS layout per process, so
  // writing the XEX's own slot value here (once semantics would want at
  // module load, harmlessly idempotent if written per-thread since it is a
  // process-wide constant, not per-thread state) is the correct value.
  if (tls_info && tls_info->index_address != 0) {
    memory.write32_be(tls_info->index_address, tls_info->slot);
  }

  out_context.kpcr_address = kpcr_address;
  out_context.tls_address = tls_address;
  out_context.tls_size = tls_size;
  return true;
}

void release_guest_thread_tls_context(memory::AddressSpace& memory,
                                      const GuestThreadTlsContext& context) noexcept {
  if (context.tls_address) {
    static_cast<void>(memory.release(context.tls_address));
  }
  if (context.kpcr_address) {
    static_cast<void>(memory.release(context.kpcr_address));
  }
}

}  // namespace xenon::core
