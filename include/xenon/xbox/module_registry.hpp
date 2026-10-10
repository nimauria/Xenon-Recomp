#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xenon/cpu/memory_port.hpp"

namespace xenon::core {
class ExportRegistry;
}
namespace xenon::memory {
class AddressSpace;
}

namespace xenon::xbox {

struct XexImage;

// Guest-visible module records and dynamic export thunks.
//
// XexGetModuleHandle hands the guest an opaque module handle; on real
// hardware that handle is the address of the module's LDR_DATA_TABLE_ENTRY in
// kernel memory, and title code (and the XEX loader ABI) reads fields out of
// it, so Xenon hands out real records at real guest addresses too. The
// executable's record is the one the XexExecutableModuleHandle kernel variable
// already points at; system modules (xboxkrnl.exe, xam.xex, xbdm.xex) get a
// record on first lookup.
//
// XexGetProcedureAddress must return an address the guest can CALL. A system
// export is host code, so the registry mints a small guest thunk address per
// (library, ordinal); XenonSession::call()/is_recognized_import_thunk() ask
// thunk_target() to route a call to such an address back into the
// ExportRegistry - exactly how a XEX's own import thunks are dispatched. The
// thunk bytes are `blr` so any code that inspects or steps over them sees
// well-formed PowerPC.
//
// Thread-safe: guest threads may resolve procedures concurrently.
class GuestModuleRegistry {
 public:
  // Where a minted thunk address dispatches to.
  struct ExportTarget {
    std::string library;
    std::uint32_t ordinal{};
  };

  enum class ProcedureResult : std::uint8_t {
    Found,
    InvalidHandle,     // hmodule is not a known module record
    OrdinalNotFound,   // no such ordinal in that module
    EntryPointNotFound // no such name in that module
  };

  GuestModuleRegistry(memory::AddressSpace& memory, const core::ExportRegistry& exports);

  // The title's own module. `record` is the guest address the
  // XexExecutableModuleHandle variable holds; `names` are the file names the
  // title may look itself up by (e.g. "default.xex"). Replaces any previous
  // executable.
  void set_executable(cpu::GuestAddress record, const XexImage& image,
                      std::vector<std::string> names);
  void clear_executable();

  // Resolves a module name (case-insensitive; a system module also matches
  // without its extension) to its handle, creating a system module's record on
  // first use. An empty name means the executable. nullopt if unknown or if
  // the executable was requested but is not loaded.
  [[nodiscard]] std::optional<cpu::GuestAddress> module_handle(std::string_view name);

  [[nodiscard]] ProcedureResult procedure_address(cpu::GuestAddress handle, std::uint32_t ordinal,
                                                  cpu::GuestAddress& out);
  [[nodiscard]] ProcedureResult procedure_address_by_name(cpu::GuestAddress handle,
                                                          std::string_view name,
                                                          cpu::GuestAddress& out);

  // Thunk dispatch (see class comment).
  [[nodiscard]] std::optional<ExportTarget> thunk_target(cpu::GuestAddress address) const;
  [[nodiscard]] bool is_thunk(cpu::GuestAddress address) const;
  [[nodiscard]] std::size_t thunk_count() const;

  // Test/diagnostic: the handle of a known module record, if any.
  [[nodiscard]] bool is_module_handle(cpu::GuestAddress handle) const;

 private:
  struct Module {
    std::string library;  // export-registry library for a system module, else empty
    cpu::GuestAddress record{};
    bool executable{};
    struct Export {
      std::string name;
      std::uint32_t ordinal{};
      cpu::GuestAddress address{};
    };
    std::vector<Export> exports;  // the executable's own exports
  };

  static constexpr std::uint32_t kThunkSize = 16u;
  static constexpr std::uint32_t kThunkPageBytes = 0x10000u;

  [[nodiscard]] Module* module_for_handle_locked(cpu::GuestAddress handle);
  [[nodiscard]] std::optional<cpu::GuestAddress> system_module_locked(const std::string& library,
                                                                      std::string_view display);
  [[nodiscard]] bool mint_thunk_locked(const std::string& library, std::uint32_t ordinal,
                                       cpu::GuestAddress& out);

  memory::AddressSpace& memory_;
  const core::ExportRegistry& exports_;
  mutable std::mutex mutex_;
  std::unordered_map<cpu::GuestAddress, Module> modules_;   // by record address
  std::vector<std::string> executable_names_;               // lowercase
  cpu::GuestAddress executable_record_{};
  std::unordered_map<cpu::GuestAddress, ExportTarget> thunk_by_address_;
  std::unordered_map<std::string, cpu::GuestAddress> thunk_by_export_;  // "lib#ord"
  cpu::GuestAddress thunk_page_{};
  std::uint32_t thunk_page_used_{kThunkPageBytes};
};

}  // namespace xenon::xbox
