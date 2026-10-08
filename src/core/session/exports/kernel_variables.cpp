#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xenon/core/session.hpp"

namespace xenon::core {

bool XenonSession::init_kernel_variable_exports() {
  if (!memory_) return false;

  // One compact guest page backs the kernel variables that retail titles are
  // allowed to import directly. Keeping the addresses in ExportRegistry makes
  // variable imports a first-class system-module contract rather than a
  // title-specific patch table.
  memory::GuestAddress page{};
  if (!memory_->allocate(memory::kBasePageSize, memory::kBasePageSize,
                         memory::kReadWrite, /*top_down=*/true, page)) {
    return false;
  }

  struct VariableSpec {
    std::uint32_t ordinal;
    const char* name;
    std::uint32_t offset;
  };
  // Xbox 360 xboxkrnl variable ordinals. These are platform ABI, not
  // title-specific data.
  constexpr VariableSpec kVariables[] = {
      {0x001Bu, "ExThreadObjectType", 0x000u},
      {0x0059u, "KeDebugMonitorData", 0x010u},
      {0x00ADu, "KeTimeStampBundle", 0x020u},
      {0x0158u, "XboxKrnlVersion", 0x040u},
      {0x0193u, "XexExecutableModuleHandle", 0x050u},
      {0x01AEu, "ExLoadedCommandLine", 0x060u},
      {0x01BEu, "VdGlobalDevice", 0x0A0u},
      {0x01C0u, "VdGpuClockInMHz", 0x0B0u},
      {0x01C1u, "VdHSIOCalibrationLock", 0x0C0u},
      {0x0266u, "KeCertMonitorData", 0x0E0u},
  };

  for (const auto& variable : kVariables) {
    VariableExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = variable.name;
    descriptor.ordinal = variable.ordinal;
    descriptor.guest_address = page + variable.offset;
    if (!export_registry_.register_variable(std::move(descriptor))) return false;
  }

  try {
    // ExThreadObjectType is an exported pointer variable. Give it a stable,
    // non-null guest object-type descriptor rather than leaving imported code
    // to dereference address zero. The descriptor is intentionally minimal;
    // Xenon's handle layer owns the actual host-side object type semantics.
    constexpr auto kThreadObjectTypeDescriptor = 0x200u;
    memory_->write32_be(page + 0x000u, page + kThreadObjectTypeDescriptor);

    // KeDebugMonitorData / KeCertMonitorData / VdGlobalDevice are valid null
    // values when the corresponding optional service/device is absent. The
    // allocation itself is nevertheless real, so importing code sees the
    // address of the exported variable, not a raw ordinal placeholder.
    memory_->write32_be(page + 0x010u, 0u);
    memory_->write32_be(page + 0x0A0u, 0u);
    memory_->write32_be(page + 0x0E0u, 0u);

    // KeTimeStampBundle is 24 bytes. It starts zeroed; time services can
    // refresh it later without changing the exported address.
    for (std::uint32_t offset = 0; offset < 24u; offset += 4u) {
      memory_->write32_be(page + 0x020u + offset, 0u);
    }

    // Retail-compatible kernel version storage. This follows the established
    // Xbox runtime convention used by recomp/emulation projects: major 2 and
    // permissive high build/revision fields for compatibility checks.
    memory_->write16_be(page + 0x040u, 2u);
    memory_->write16_be(page + 0x042u, 0xFFFFu);
    memory_->write16_be(page + 0x044u, 0xFFFFu);
    memory_->write8(page + 0x046u, 0x80u);
    memory_->write8(page + 0x047u, 0x00u);

    // XexExecutableModuleHandle is a pointer variable. Back it with a small
    // stable module record now; refresh_dynamic_kernel_variables() fills the
    // XEX-header pointer once the effective image has been loaded.
    constexpr auto kExecutableModuleRecord = 0x100u;
    memory_->write32_be(page + 0x050u, page + kExecutableModuleRecord);

    static constexpr char kCommandLine[] = "\"default.xex\"";
    std::vector<std::byte> command_line(sizeof(kCommandLine));
    for (std::size_t i = 0; i < sizeof(kCommandLine); ++i) {
      command_line[i] = static_cast<std::byte>(kCommandLine[i]);
    }
    memory_->write_bytes(page + 0x060u, command_line);

    // Xenos nominal GPU clock is 500 MHz.
    memory_->write32_be(page + 0x0B0u, 500u);

    // VdHSIOCalibrationLock is an RTL critical section (28 bytes). Initialize
    // the fields used by the Xbox runtime: synchronization-event type, spin
    // count / 256, signal state 0, lock count -1, recursion 0, owner 0.
    memory_->write8(page + 0x0C0u, 1u);
    memory_->write8(page + 0x0C1u, static_cast<std::uint8_t>((10000u + 255u) >> 8u));
    memory_->write32_be(page + 0x0C4u, 0u);
    memory_->write32_be(page + 0x0D0u, 0xFFFFFFFFu);
    memory_->write32_be(page + 0x0D4u, 0u);
    memory_->write32_be(page + 0x0D8u, 0u);
  } catch (const memory::MemoryFault&) {
    return false;
  }

  return true;
}

bool XenonSession::bind_xex_variable_imports() {
  if (!loaded_xex_ || !memory_) return false;

  for (const auto& import : loaded_xex_->image.imports) {
    if (!import.is_variable()) continue;

    std::optional<cpu::GuestAddress> variable;
    if (!import.symbol.empty()) {
      variable = export_registry_.resolve_variable(import.module, import.symbol);
    }
    if (!variable) {
      variable = export_registry_.resolve_variable(import.module, import.ordinal);
    }
    if (!variable) continue;  // Kept as an unresolved compatibility diagnostic.

    const auto mapping = memory_->query(import.guest_thunk);
    if (!mapping || mapping->state != memory::PageState::Committed || mapping->page_size == 0u) {
      set_error("Variable import slot is not mapped/committed: " + import.module +
                " ordinal " + std::to_string(import.ordinal));
      return false;
    }

    const auto page_size = mapping->page_size;
    const auto page_base = import.guest_thunk - (import.guest_thunk % page_size);
    const auto original_protect = mapping->current_protect;
    const auto writable_protect = original_protect | memory::Protect::Write;
    if (writable_protect != original_protect &&
        !memory_->protect(page_base, page_size, writable_protect)) {
      set_error("Failed to make variable import page writable: " + import.module +
                " ordinal " + std::to_string(import.ordinal));
      return false;
    }

    bool wrote = false;
    try {
      memory_->write32_be(import.guest_thunk, *variable);
      wrote = true;
    } catch (const memory::MemoryFault&) {
      wrote = false;
    }

    if (writable_protect != original_protect) {
      if (!memory_->protect(page_base, page_size, original_protect)) {
        set_error("Failed to restore variable import page protection: " + import.module +
                  " ordinal " + std::to_string(import.ordinal));
        return false;
      }
    }
    if (!wrote) {
      set_error("Failed to bind variable import: " + import.module + " ordinal " +
                std::to_string(import.ordinal));
      return false;
    }
  }
  return true;
}

bool XenonSession::refresh_dynamic_kernel_variables() {
  if (!memory_ || !loaded_xex_) return false;
  const auto module_handle_storage =
      export_registry_.resolve_variable("xboxkrnl.exe", 0x0193u);
  if (!module_handle_storage) return true;

  try {
    const auto module_record = memory_->read32_be(*module_handle_storage);
    if (module_record == 0u) return false;

    // Preserve a guest copy of the effective XEX header and expose its address
    // at the loader-record field used by Xbox code that queries XEX optional
    // headers. This is deliberately a minimal loader record; KernelModule is
    // still the canonical host-side module object.
    if (!loaded_xex_->image.header_bytes.empty()) {
      memory::GuestAddress header_copy{};
      const auto header_size = static_cast<std::uint32_t>(loaded_xex_->image.header_bytes.size());
      if (!memory_->allocate(header_size, 16u, memory::kReadWrite,
                             /*top_down=*/true, header_copy)) {
        return false;
      }
      memory_->write_bytes(header_copy, loaded_xex_->image.header_bytes);
      memory_->write32_be(module_record + 0x58u, header_copy);
    }
    if (module_registry_) {
      // The title's own module: reachable through XexGetModuleHandle(NULL) and by
      // its file name, and its exports through XexGetProcedureAddress.
      module_registry_->set_executable(module_record, loaded_xex_->image, {"default.xex"});
    }
  } catch (const memory::MemoryFault&) {
    return false;
  }
  return true;
}

}  // namespace xenon::core
