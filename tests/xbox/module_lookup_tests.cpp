// XexGetModuleHandle / XexGetProcedureAddress and the GuestModuleRegistry that
// backs them: guest-visible module records, callable export thunks for system
// modules, and the executable's own export table.
//
// Exports are driven through core::ExportRegistry::invoke() with real guest
// memory (real ordinals, real ExportCallContext), checking Xbox-visible
// results: the Win32 error / NTSTATUS in r3 and the values written to guest
// memory. Ace Combat 6's CRT imports both exports.

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <string>

#include "xenon/core/export_registry.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/module_registry.hpp"
#include "xenon/xbox/xboxkrnl_xex_module_exports.hpp"
#include "xenon/xbox/xex_loader.hpp"

using namespace xenon;

namespace {

constexpr std::uint32_t kOrdGetModuleHandle = 0x195u;
constexpr std::uint32_t kOrdGetProcedureAddress = 0x197u;
constexpr std::uint32_t kErrorNotFound = 0x490u;
constexpr std::uint32_t kErrorInvalidParameter = 0x57u;
constexpr std::uint32_t kStatusInvalidHandle = 0xC0000008u;
constexpr std::uint32_t kStatusInvalidParameter = 0xC000000Du;
constexpr std::uint32_t kStatusOrdinalNotFound = 0xC0000262u;
constexpr std::uint32_t kStatusEntrypointNotFound = 0xC0000263u;
constexpr std::uint32_t kBlr = 0x4E800020u;

struct Fixture {
  std::shared_ptr<memory::AddressSpace> address_space;
  core::ExportRegistry exports;
  std::unique_ptr<xbox::GuestModuleRegistry> modules;
  core::ExportRegistry guest_abi;  // registry of the two handlers under test
  memory::GuestAddress arena{};
  std::uint32_t arena_used{};
  memory::GuestAddress exe_record{};

  Fixture() {
    address_space = std::make_shared<memory::AddressSpace>(memory::GuestTranslationMode::Compact);
    const bool ok = address_space->initialize();
    assert(ok);
    static_cast<void>(ok);
    modules = std::make_unique<xbox::GuestModuleRegistry>(*address_space, exports);

    // A few real-looking system exports and one variable.
    add_export("xboxkrnl.exe", 0x0CCu, "NtAllocateVirtualMemory");
    add_export("xboxkrnl.exe", 0x0DCu, "NtFreeVirtualMemory");
    add_export("xam.xex", 0x0211u, "XamSomething");
    core::VariableExportDescriptor variable{};
    variable.library = "xboxkrnl.exe";
    variable.name = "KeTimeStampBundle";
    variable.ordinal = 0xADu;
    variable.guest_address = 0x20000000u;
    const bool var_ok = exports.register_variable(std::move(variable));
    assert(var_ok);
    static_cast<void>(var_ok);

    // The two exports under test, bound to `modules` exactly as the session does.
    register_abi(kOrdGetModuleHandle, "XexGetModuleHandle", true);
    register_abi(kOrdGetProcedureAddress, "XexGetProcedureAddress", false);

    const bool arena_ok = address_space->allocate(0x10000u, 0x1000u, memory::kReadWrite, true, arena);
    assert(arena_ok);
    static_cast<void>(arena_ok);
    const bool exe_ok = address_space->allocate(0x1000u, 0x1000u, memory::kReadWrite, true, exe_record);
    assert(exe_ok);
    static_cast<void>(exe_ok);
  }

  void add_export(const char* library, std::uint32_t ordinal, const char* name) {
    core::ExportDescriptor descriptor{};
    descriptor.library = library;
    descriptor.name = name;
    descriptor.ordinal = ordinal;
    descriptor.handler = [](core::ExportCallContext&) { return true; };
    const bool ok = exports.register_export(std::move(descriptor));
    assert(ok);
    static_cast<void>(ok);
  }

  void register_abi(std::uint32_t ordinal, const char* name, bool module_handle) {
    core::ExportDescriptor descriptor{};
    descriptor.library = "xboxkrnl.exe";
    descriptor.name = name;
    descriptor.ordinal = ordinal;
    xbox::GuestModuleRegistry* raw = modules.get();
    if (module_handle) {
      descriptor.handler = [raw](core::ExportCallContext& ctx) {
        return xbox::xex_get_module_handle_export(*raw, ctx);
      };
    } else {
      descriptor.handler = [raw](core::ExportCallContext& ctx) {
        return xbox::xex_get_procedure_address_export(*raw, ctx);
      };
    }
    const bool ok = guest_abi.register_export(std::move(descriptor));
    assert(ok);
    static_cast<void>(ok);
  }

  memory::GuestAddress scratch(std::uint32_t bytes = 16) {
    const auto aligned = (bytes + 15u) & ~15u;
    assert(arena_used + aligned <= 0x10000u);
    const auto addr = arena + arena_used;
    arena_used += aligned;
    return addr;
  }

  memory::GuestAddress cstring(const std::string& text) {
    const auto addr = scratch(static_cast<std::uint32_t>(text.size()) + 1u);
    for (std::size_t i = 0; i < text.size(); ++i) {
      address_space->write8(addr + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(text[i]));
    }
    address_space->write8(addr + static_cast<std::uint32_t>(text.size()), 0u);
    return addr;
  }

  std::uint64_t call(std::uint32_t ordinal, std::uint64_t r3, std::uint64_t r4, std::uint64_t r5 = 0) {
    cpu::CpuState cpu{};
    cpu.gpr[3] = r3;
    cpu.gpr[4] = r4;
    cpu.gpr[5] = r5;
    core::ExportCallContext ctx{cpu, *address_space, 0, 0};
    const auto result = guest_abi.invoke("xboxkrnl", ordinal, ctx);
    assert(result.handled && result.success);
    return cpu.gpr[3];
  }

  void load_executable() {
    xbox::XexImage image{};
    image.original_pe_name = "AceCombat6.exe";
    image.exports.push_back({"Init", 1u, 0x82001000u, 0u});
    image.exports.push_back({"Shutdown", 7u, 0x82002000u, 0u});
    image.exports.push_back({"Unresolved", 9u, 0u, 0u});
    modules->set_executable(exe_record, image, {"default.xex"});
  }
};

// A module name resolves case-insensitively, by bare name, by alias without an
// extension and by device path; the same module always yields the same handle
// and the handle is a real record carrying the module's name.
void test_system_module_handles() {
  Fixture f;
  const auto krnl = f.modules->module_handle("xboxkrnl.exe");
  assert(krnl && *krnl != 0u);
  assert(f.modules->module_handle("XBOXKRNL.EXE") == krnl);
  assert(f.modules->module_handle("xboxkrnl") == krnl);
  assert(f.modules->module_handle("\\Device\\Harddisk0\\Partition1\\XBOXKRNL.exe") == krnl);
  const auto xam = f.modules->module_handle("xam.xex");
  assert(xam && *xam != *krnl);
  assert(f.modules->is_module_handle(*krnl) && f.modules->is_module_handle(*xam));
  assert(!f.modules->module_handle("nosuchmodule.dll"));
  assert(!f.modules->is_module_handle(0x12345678u));

  // The record's name fields describe the module (UTF-16BE "xboxkrnl.exe").
  const auto record = *krnl;
  const auto full_name_length = f.address_space->read16_be(record + 0x24u);
  const auto full_name_buffer = f.address_space->read32_be(record + 0x24u + 4u);
  assert(full_name_length == std::string("xboxkrnl.exe").size() * 2u);
  assert(f.address_space->read16_be(full_name_buffer) == u'x');
  assert(f.address_space->read16_be(full_name_buffer + 2u) == u'b');
  assert(f.address_space->read16_be(record + 0x40u) == 1u && "load count");
}

// With no executable loaded, "the calling executable" cannot be resolved; once
// loaded it resolves by NULL name, by file name and by the PE name.
void test_executable_handle() {
  Fixture f;
  assert(!f.modules->module_handle(""));
  f.load_executable();
  assert(f.modules->module_handle("") == f.exe_record);
  assert(f.modules->module_handle("Default.XEX") == f.exe_record);
  assert(f.modules->module_handle("acecombat6.exe") == f.exe_record);
  f.modules->clear_executable();
  assert(!f.modules->module_handle(""));
  assert(!f.modules->is_module_handle(f.exe_record));
}

// The executable's exports resolve to their real image addresses by ordinal
// and name; a missing ordinal/name, and an export with no address, do not.
void test_executable_exports() {
  Fixture f;
  f.load_executable();
  cpu::GuestAddress out{};
  using R = xbox::GuestModuleRegistry::ProcedureResult;
  assert(f.modules->procedure_address(f.exe_record, 1u, out) == R::Found && out == 0x82001000u);
  assert(f.modules->procedure_address(0u, 7u, out) == R::Found && out == 0x82002000u &&
         "handle 0 means the executable");
  assert(f.modules->procedure_address_by_name(f.exe_record, "Shutdown", out) == R::Found &&
         out == 0x82002000u);
  assert(f.modules->procedure_address(f.exe_record, 2u, out) == R::OrdinalNotFound);
  assert(f.modules->procedure_address(f.exe_record, 9u, out) == R::OrdinalNotFound);
  assert(f.modules->procedure_address_by_name(f.exe_record, "Nope", out) == R::EntryPointNotFound);
  assert(f.modules->procedure_address(0x77777777u, 1u, out) == R::InvalidHandle);
}

// A system function resolves to a stable, callable thunk that dispatches back
// to (library, ordinal); the thunk holds well-formed `blr` code; a variable
// resolves to its own address; an unregistered ordinal/name is not found.
void test_system_procedure_thunks() {
  Fixture f;
  const auto krnl = *f.modules->module_handle("xboxkrnl.exe");
  using R = xbox::GuestModuleRegistry::ProcedureResult;
  cpu::GuestAddress alloc_thunk{};
  assert(f.modules->procedure_address(krnl, 0x0CCu, alloc_thunk) == R::Found);
  assert(alloc_thunk != 0u && f.modules->is_thunk(alloc_thunk));
  const auto target = f.modules->thunk_target(alloc_thunk);
  assert(target && target->library == "xboxkrnl.exe" && target->ordinal == 0x0CCu);
  for (std::uint32_t word = 0; word < 16u; word += 4u) {
    assert(f.address_space->read32_be(alloc_thunk + word) == kBlr);
  }

  cpu::GuestAddress again{};
  assert(f.modules->procedure_address(krnl, 0x0CCu, again) == R::Found && again == alloc_thunk &&
         "the same export always yields the same thunk");
  cpu::GuestAddress by_name{};
  assert(f.modules->procedure_address_by_name(krnl, "NtAllocateVirtualMemory", by_name) == R::Found &&
         by_name == alloc_thunk);
  cpu::GuestAddress free_thunk{};
  assert(f.modules->procedure_address(krnl, 0x0DCu, free_thunk) == R::Found &&
         free_thunk != alloc_thunk);
  assert(f.modules->thunk_count() == 2u);

  cpu::GuestAddress variable{};
  assert(f.modules->procedure_address(krnl, 0xADu, variable) == R::Found && variable == 0x20000000u);
  assert(f.modules->procedure_address_by_name(krnl, "KeTimeStampBundle", variable) == R::Found &&
         variable == 0x20000000u);
  assert(!f.modules->is_thunk(variable));

  cpu::GuestAddress none{};
  assert(f.modules->procedure_address(krnl, 0x3FFFu, none) == R::OrdinalNotFound);
  assert(f.modules->procedure_address_by_name(krnl, "NoSuchExport", none) == R::EntryPointNotFound);
  assert(f.modules->thunk_count() == 2u && "a failed lookup must not mint a thunk");

  // Exports are per module: xam's ordinal is not visible through xboxkrnl.
  const auto xam = *f.modules->module_handle("xam.xex");
  assert(f.modules->procedure_address(krnl, 0x0211u, none) == R::OrdinalNotFound);
  cpu::GuestAddress xam_thunk{};
  assert(f.modules->procedure_address(xam, 0x0211u, xam_thunk) == R::Found);
  assert(f.modules->thunk_target(xam_thunk)->library == "xam.xex");
}

// More thunks than fit in one page roll over to a fresh page with no address
// reuse.
void test_thunk_page_rollover() {
  Fixture f;
  for (std::uint32_t ordinal = 1000u; ordinal < 1000u + 5000u; ++ordinal) {
    f.add_export("xboxkrnl.exe", ordinal, ("Bulk" + std::to_string(ordinal)).c_str());
  }
  const auto krnl = *f.modules->module_handle("xboxkrnl.exe");
  std::set<cpu::GuestAddress> seen;
  for (std::uint32_t ordinal = 1000u; ordinal < 1000u + 5000u; ++ordinal) {
    cpu::GuestAddress thunk{};
    assert(f.modules->procedure_address(krnl, ordinal, thunk) ==
           xbox::GuestModuleRegistry::ProcedureResult::Found);
    assert(seen.insert(thunk).second && "every export gets its own thunk address");
    assert(f.modules->thunk_target(thunk)->ordinal == ordinal);
  }
  assert(f.modules->thunk_count() == 5000u);
}

// XexGetModuleHandle guest ABI.
void test_get_module_handle_abi() {
  Fixture f;
  f.load_executable();
  const auto out = f.scratch();

  // NULL name = the executable.
  f.address_space->write32_be(out, 0xDEADBEEFu);
  assert(f.call(kOrdGetModuleHandle, 0u, out) == 0u);
  assert(f.address_space->read32_be(out) == f.exe_record);

  // Named system module.
  assert(f.call(kOrdGetModuleHandle, f.cstring("xam.xex"), out) == 0u);
  const auto xam = f.address_space->read32_be(out);
  assert(xam != 0u && f.modules->is_module_handle(xam));

  // Unknown module: ERROR_NOT_FOUND (a Win32 error, not an NTSTATUS) and the
  // out handle is cleared.
  f.address_space->write32_be(out, 0xDEADBEEFu);
  assert(f.call(kOrdGetModuleHandle, f.cstring("missing.dll"), out) == kErrorNotFound);
  assert(f.address_space->read32_be(out) == 0u);

  // A non-NULL pointer to an empty string is not the executable.
  assert(f.call(kOrdGetModuleHandle, f.cstring(""), out) == kErrorNotFound);

  // NULL out pointer.
  assert(f.call(kOrdGetModuleHandle, 0u, 0u) == kErrorInvalidParameter);
}

// XexGetProcedureAddress guest ABI, by ordinal and by name pointer.
void test_get_procedure_address_abi() {
  Fixture f;
  f.load_executable();
  const auto out = f.scratch();
  const auto krnl_out = f.scratch();
  assert(f.call(kOrdGetModuleHandle, f.cstring("xboxkrnl.exe"), krnl_out) == 0u);
  const auto krnl = f.address_space->read32_be(krnl_out);

  // By ordinal.
  assert(f.call(kOrdGetProcedureAddress, krnl, 0x0CCu, out) == 0u);
  const auto thunk = f.address_space->read32_be(out);
  assert(f.modules->is_thunk(thunk));
  // By name pointer (high bits set -> a pointer, not an ordinal).
  f.address_space->write32_be(out, 0u);
  assert(f.call(kOrdGetProcedureAddress, krnl, f.cstring("NtAllocateVirtualMemory"), out) == 0u);
  assert(f.address_space->read32_be(out) == thunk);

  // The executable's own export via handle 0.
  assert(f.call(kOrdGetProcedureAddress, 0u, 1u, out) == 0u);
  assert(f.address_space->read32_be(out) == 0x82001000u);

  // Error statuses, each clearing the out value.
  f.address_space->write32_be(out, 0xDEADBEEFu);
  assert(f.call(kOrdGetProcedureAddress, 0x77777777u, 1u, out) == kStatusInvalidHandle);
  assert(f.address_space->read32_be(out) == 0u);
  f.address_space->write32_be(out, 0xDEADBEEFu);
  assert(f.call(kOrdGetProcedureAddress, krnl, 0x3FFFu, out) == kStatusOrdinalNotFound);
  assert(f.address_space->read32_be(out) == 0u);
  assert(f.call(kOrdGetProcedureAddress, krnl, f.cstring("NoSuchExport"), out) ==
         kStatusEntrypointNotFound);
  assert(f.call(kOrdGetProcedureAddress, krnl, 0x0CCu, 0u) == kStatusInvalidParameter);
}

}  // namespace

int main() {
  std::cout << "Testing XexGetModuleHandle/XexGetProcedureAddress...\n";
  test_system_module_handles();
  test_executable_handle();
  test_executable_exports();
  test_system_procedure_thunks();
  test_thunk_page_rollover();
  test_get_module_handle_abi();
  test_get_procedure_address_abi();
  std::cout << "All module lookup tests passed!\n";
  return 0;
}
