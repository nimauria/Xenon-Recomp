#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "xenon/filesystem/host_path_device.hpp"
#include "xenon/filesystem/virtual_file_system.hpp"
#include "xenon/kernel/xbox_io.hpp"
#include "xenon/kernel/xbox_io_guest.hpp"
#include "xenon/memory/address_space.hpp"
#include "xenon/xbox/imports.hpp"

namespace fs = xenon::filesystem;
namespace kernel = xenon::kernel;
namespace kx = xenon::kernel::xbox;
namespace memory = xenon::memory;
namespace xbox = xenon::xbox;

namespace {
constexpr memory::GuestAddress kBase = 0x00100000u;
constexpr memory::GuestAddress kAnsi = kBase + 0x100u;
constexpr memory::GuestAddress kObject = kBase + 0x120u;
constexpr memory::GuestAddress kHandle = kBase + 0x140u;
constexpr memory::GuestAddress kIosb = kBase + 0x148u;
constexpr memory::GuestAddress kPath = kBase + 0x200u;
constexpr memory::GuestAddress kBuffer = kBase + 0x1000u;
constexpr memory::GuestAddress kStack = kBase + 0x8000u;

class TempDirectory {
 public:
  TempDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("xenon_imports_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path_);
  }
  ~TempDirectory() { std::error_code ec; std::filesystem::remove_all(path_, ec); }
  const auto& path() const { return path_; }
 private:
  std::filesystem::path path_{};
};

std::shared_ptr<fs::VirtualFileSystem> make_vfs(const std::filesystem::path& root) {
  auto vfs = std::make_shared<fs::VirtualFileSystem>();
  fs::HostPathDeviceOptions options{};
  options.read_only = false;
  options.create_root = true;
  auto device = std::make_shared<fs::HostPathDevice>(
      "\\Device\\Harddisk0\\Partition1", root, options);
  assert(vfs->register_device(device) == fs::FsError::None);
  assert(vfs->register_symbolic_link("game:", device->mount_point()) == fs::FsError::None);
  return vfs;
}

void write_ansi(memory::AddressSpace& mem, std::string_view value) {
  mem.write16_be(kAnsi + 0, static_cast<std::uint16_t>(value.size()));
  mem.write16_be(kAnsi + 2, static_cast<std::uint16_t>(value.size() + 1));
  mem.write32_be(kAnsi + 4, kPath);
  mem.write_bytes(kPath, std::as_bytes(std::span(value.data(), value.size())));
  mem.write8(kPath + static_cast<std::uint32_t>(value.size()), 0);
  mem.write32_be(kObject + 0, kernel::kInvalidHandle);
  mem.write32_be(kObject + 4, kAnsi);
  mem.write32_be(kObject + 8, 0x40u);
}

void set_arg(xenon::cpu::CpuState& state, memory::AddressSpace& mem,
             std::size_t index, std::uint32_t value) {
  if (index < 8) {
    state.gpr[3 + index] = value;
  } else {
    const auto address = static_cast<std::uint32_t>(state.gpr[1]) + 0x54u +
                         static_cast<std::uint32_t>((index - 8) * 8u);
    mem.write32_be(address, value);
  }
}

void test_registry_and_real_ppc_thunks() {
  TempDirectory temp;
  {
    std::ofstream out(temp.path() / "data.bin", std::ios::binary);
    out << "abcdef";
  }

  memory::AddressSpace mem(memory::GuestTranslationMode::Compact);
  assert(mem.initialize());
  assert(mem.commit_fixed(kBase, 0x10000u, memory::kReadWrite));
  kernel::KernelIoManager io(make_vfs(temp.path()));
  kx::GuestIoBridge bridge(mem, io);

  xbox::ImportRegistry registry;
  assert(xbox::register_xboxkrnl_io_imports(registry));
  assert(xbox::register_xboxkrnl_io_imports(registry));
  assert(registry.enumerate("xboxkrnl.exe").size() == 11);
  assert(registry.resolve("XBOXKRNL.EXE", "ntcreatefile") != nullptr);
  assert(registry.resolve("xboxkrnl", 0x00D2) != nullptr);
  assert(registry.resolve("xboxkrnl", 0x00FF) != nullptr);
  assert(registry.resolve("xam", 0x00D2) == nullptr);

  write_ansi(mem, "game:\\data.bin");
  xenon::cpu::CpuState state{};
  state.gpr[1] = kStack;
  set_arg(state, mem, 0, kHandle);
  set_arg(state, mem, 1, kx::access::GenericRead);
  set_arg(state, mem, 2, kObject);
  set_arg(state, mem, 3, kIosb);
  set_arg(state, mem, 4, 0);
  set_arg(state, mem, 5, 0);
  set_arg(state, mem, 6, kx::share::All);
  set_arg(state, mem, 7, 1);  // FILE_OPEN
  set_arg(state, mem, 8, kx::create_option::SynchronousIoNonAlert);

  xbox::ImportCallContext context{state, mem, bridge};
  assert(registry.invoke("xboxkrnl.exe", 0x00D2, context));
  assert(static_cast<std::uint32_t>(state.gpr[3]) == kx::status::Success);
  const auto handle = mem.read32_be(kHandle);
  assert(handle != kernel::kInvalidHandle);
  assert(mem.read32_be(kIosb) == kx::status::Success);
  assert(mem.read32_be(kIosb + 4) == static_cast<std::uint32_t>(kx::FileAction::Opened));

  // Invoke NtReadFile by name using the normal register-only 8-argument form.
  state = {};
  state.gpr[1] = kStack;
  set_arg(state, mem, 0, handle);
  set_arg(state, mem, 1, kernel::kInvalidHandle);
  set_arg(state, mem, 2, 0);
  set_arg(state, mem, 3, 0);
  set_arg(state, mem, 4, kIosb);
  set_arg(state, mem, 5, kBuffer);
  set_arg(state, mem, 6, 3);
  set_arg(state, mem, 7, 0);
  assert(registry.invoke("XBOXKRNL", "NtReadFile", context));
  assert(static_cast<std::uint32_t>(state.gpr[3]) == kx::status::Success);
  assert(mem.read32_be(kIosb + 4) == 3);
  assert(mem.read8(kBuffer + 0) == 'a');
  assert(mem.read8(kBuffer + 1) == 'b');
  assert(mem.read8(kBuffer + 2) == 'c');
  assert(io.close(handle) == kernel::KernelIoCode::Success);
}

void test_bad_stack_argument_returns_access_violation() {
  TempDirectory temp;
  memory::AddressSpace mem(memory::GuestTranslationMode::Compact);
  assert(mem.initialize());
  assert(mem.commit_fixed(kBase, 0x4000u, memory::kReadWrite));
  kernel::KernelIoManager io(make_vfs(temp.path()));
  kx::GuestIoBridge bridge(mem, io);
  xbox::ImportRegistry registry;
  assert(xbox::register_xboxkrnl_io_imports(registry));

  xenon::cpu::CpuState state{};
  state.gpr[1] = 0x00010000u;  // uncommitted stack: ninth NtCreateFile argument faults.
  xbox::ImportCallContext context{state, mem, bridge};
  assert(registry.invoke("xboxkrnl", 0x00D2, context));
  assert(static_cast<std::uint32_t>(state.gpr[3]) == kx::status::AccessViolation);
}
}  // namespace

int main() {
  test_registry_and_real_ppc_thunks();
  test_bad_stack_argument_returns_access_violation();
  return 0;
}
