#include "xenon/core/session.hpp"

#include "xenon/core/call_bridge.hpp"
#include "xenon/core/guest_thread_context.hpp"
#include "xenon/cpu/flat_memory.hpp"
#include "xenon/memory/address_space.hpp"

#include <cassert>
#include <cstring>
#include <iostream>

int main() {
  std::cout << "Testing Xenon Session...\n";

  // Test 0: guest_thread_context - KPCR + static TLS setup. Two independent
  // calls with the SAME XexTls template must produce two fully independent
  // (non-overlapping) TLS instances, each correctly primed from the shared
  // template, while gpr[13] (the KPCR address) differs per thread. This is
  // the free-function-level proof for "two threads receive distinct TLS
  // instances while sharing the same template definition" - see
  // create_guest_process() for how a real guest thread's CpuState::gpr[13]
  // is set from this.
  {
    xenon::memory::AddressSpace memory(xenon::memory::GuestTranslationMode::Auto);
    assert(memory.initialize());

    // A small guest-resident "template" region standing in for the XEX's
    // own .tls-adjacent data (raw_data_start must be a real, already-mapped
    // guest address, matching how the effective image is mapped by the time
    // create_guest_process() runs this against real XEX Loader V2 output).
    xenon::memory::GuestAddress template_address{};
    assert(memory.allocate(64, 16, xenon::memory::kReadWrite, false, template_address));
    for (std::uint32_t i = 0; i < 16; ++i) {
      memory.write8(template_address + i, static_cast<std::uint8_t>(0xA0 + i));
    }

    xenon::xbox::XexTls tls_template{};
    tls_template.slot = 0;
    tls_template.raw_data_start = template_address;
    tls_template.raw_data_size = 16;   // Initialized prefix.
    tls_template.data_size = 64;       // Total per-thread block (zero-filled tail).

    xenon::memory::GuestAddress stack_a{};
    xenon::memory::GuestAddress stack_b{};
    assert(memory.allocate(4096, 16, xenon::memory::kReadWrite, true, stack_a));
    assert(memory.allocate(4096, 16, xenon::memory::kReadWrite, true, stack_b));

    xenon::core::GuestThreadTlsContext ctx_a{};
    xenon::core::GuestThreadTlsContext ctx_b{};
    std::string setup_error;
    assert(xenon::core::setup_guest_thread_tls_context(memory, tls_template, stack_a,
                                                       4096, ctx_a, &setup_error));
    assert(xenon::core::setup_guest_thread_tls_context(memory, tls_template, stack_b,
                                                       4096, ctx_b, &setup_error));

    assert(ctx_a.kpcr_address != 0 && ctx_b.kpcr_address != 0);
    assert(ctx_a.tls_address != 0 && ctx_b.tls_address != 0);
    assert(ctx_a.kpcr_address != ctx_b.kpcr_address &&
           "Two threads must get distinct KPCR blocks");
    assert(ctx_a.tls_address != ctx_b.tls_address &&
           "Two threads must get distinct TLS instances from the same template");

    // Both instances must be correctly primed from the same template:
    // raw_data_size bytes copied, remainder zero-filled.
    for (const auto& ctx : {ctx_a, ctx_b}) {
      for (std::uint32_t i = 0; i < 16; ++i) {
        assert(memory.read8(ctx.tls_address + i) == static_cast<std::uint8_t>(0xA0 + i));
      }
      for (std::uint32_t i = 16; i < 64; ++i) {
        assert(memory.read8(ctx.tls_address + i) == 0);
      }
    }

    // KPCR fields: tls_ptr must point at this thread's own TLS instance
    // (not the other thread's), self-pointer must be the KPCR's own
    // address, and stack base/limit must match the stack this call was
    // given - all real, guest-readable values a recompiled function's
    // r13-relative loads would see.
    assert(memory.read32_be(ctx_a.kpcr_address + xenon::core::GuestKpcrLayout::kTlsPtrOffset) ==
           ctx_a.tls_address);
    assert(memory.read32_be(ctx_b.kpcr_address + xenon::core::GuestKpcrLayout::kTlsPtrOffset) ==
           ctx_b.tls_address);
    assert(memory.read32_be(ctx_a.kpcr_address + xenon::core::GuestKpcrLayout::kSelfOffset) ==
           ctx_a.kpcr_address);
    assert(memory.read32_be(ctx_a.kpcr_address + xenon::core::GuestKpcrLayout::kStackBaseOffset) ==
           stack_a);
    assert(memory.read32_be(ctx_a.kpcr_address + xenon::core::GuestKpcrLayout::kStackLimitOffset) ==
           stack_a - 4096u);

    xenon::core::release_guest_thread_tls_context(memory, ctx_a);
    xenon::core::release_guest_thread_tls_context(memory, ctx_b);
  }

  // Test 0b: a title with no compiler-emitted TLS at all (image.tls is
  // empty - the common case for most Xbox 360 titles) must still get a real
  // KPCR (for stack-limit/self fields), just no TLS block.
  {
    xenon::memory::AddressSpace memory(xenon::memory::GuestTranslationMode::Auto);
    assert(memory.initialize());
    xenon::memory::GuestAddress stack{};
    assert(memory.allocate(4096, 16, xenon::memory::kReadWrite, true, stack));

    xenon::core::GuestThreadTlsContext ctx{};
    std::string setup_error;
    assert(xenon::core::setup_guest_thread_tls_context(
        memory, std::optional<xenon::xbox::XexTls>{}, stack, 4096, ctx, &setup_error));
    assert(ctx.kpcr_address != 0);
    assert(ctx.tls_address == 0 && "No XexTls -> no TLS block allocated");
    assert(memory.read32_be(ctx.kpcr_address + xenon::core::GuestKpcrLayout::kTlsPtrOffset) == 0);

    xenon::core::release_guest_thread_tls_context(memory, ctx);
  }

  // Test 1: Create and initialize session
  {
    xenon::core::XenonSession session;
    
    xenon::core::SessionConfig config{};
    config.enable_logging = false;
    config.enable_graphics = false;
    config.enable_input = false;
    
    auto result = session.initialize(config);
    assert(result.success && "Session initialization should succeed");
    assert(session.is_initialized() && "Session should be initialized");
    assert(session.state() == xenon::core::SessionState::Ready);
    
    // Verify subsystems are created
    assert(session.memory() != nullptr && "Memory subsystem should exist");
    assert(session.filesystem() != nullptr && "Filesystem should exist");
    assert(session.kernel_io() != nullptr && "Kernel I/O should exist");
    assert(session.exports() != nullptr && "Export registry should exist");
    
    session.shutdown();
    assert(!session.is_initialized() && "Session should not be initialized after shutdown");
  }

  // Test 1b: Session can be re-initialized after a clean shutdown (this only
  // works because shutdown() lands back on SessionState::Uninitialized).
  {
    xenon::core::XenonSession session;
    xenon::core::SessionConfig config{};
    config.enable_logging = false;
    config.enable_graphics = false;
    config.enable_input = false;

    assert(session.initialize(config).success);
    session.shutdown();
    auto second = session.initialize(config);
    assert(second.success && "Session should accept re-initialization after shutdown");
    assert(session.is_initialized());
    session.shutdown();
  }

  // Test: explicit "null" graphics backend is accepted (headless/test mode).
  {
    xenon::core::XenonSession session;
    xenon::core::SessionConfig config{};
    config.enable_logging = false;
    config.enable_graphics = true;
    config.graphics_backend = "Null";
    config.enable_input = false;

    auto result = session.initialize(config);
    assert(result.success && "Explicit Null graphics backend should be accepted");
    assert(session.gpu() != nullptr && "gpu() should be populated for an explicit Null backend");
    session.shutdown();
  }

  // Test: an unrecognized graphics backend name must fail initialization
  // rather than silently falling back to Null.
  {
    xenon::core::XenonSession session;
    xenon::core::SessionConfig config{};
    config.enable_logging = false;
    config.enable_graphics = true;
    config.graphics_backend = "not-a-real-backend";
    config.enable_input = false;

    auto result = session.initialize(config);
    assert(!result.success && "Unknown graphics backend must fail, not fall back to Null");
    assert(session.state() == xenon::core::SessionState::Failed);
  }

  // Test: "Automatic" graphics backend selection either produces a real
  // native backend on a build that has one compiled in, or fails outright -
  // it must never silently produce a Null backend.
#if defined(XENON_HAS_VULKAN) || defined(XENON_HAS_D3D12)
  {
    xenon::core::XenonSession session;
    xenon::core::SessionConfig config{};
    config.enable_logging = false;
    config.enable_graphics = true;
    config.graphics_backend = "Automatic";
    config.enable_input = false;

    auto result = session.initialize(config);
    assert(result.success && "Automatic graphics backend should select a real native backend");
    assert(session.gpu() != nullptr);
    session.shutdown();
  }
#endif

#if defined(XENON_HAS_VULKAN)
  // Test: explicit Vulkan backend selection.
  {
    xenon::core::XenonSession session;
    xenon::core::SessionConfig config{};
    config.enable_logging = false;
    config.enable_graphics = true;
    config.graphics_backend = "Vulkan";
    config.enable_input = false;

    auto result = session.initialize(config);
    assert(result.success && "Explicit Vulkan backend should initialize successfully");
    assert(session.gpu() != nullptr);
    session.shutdown();
  }
#endif

#if defined(XENON_HAS_D3D12)
  // Test: explicit D3D12 backend selection.
  {
    xenon::core::XenonSession session;
    xenon::core::SessionConfig config{};
    config.enable_logging = false;
    config.enable_graphics = true;
    config.graphics_backend = "D3D12";
    config.enable_input = false;

    auto result = session.initialize(config);
    assert(result.success && "Explicit D3D12 backend should initialize successfully");
    assert(session.gpu() != nullptr);
    session.shutdown();
  }
#endif

  // Test: normal input initialization must not produce a zero-provider
  // input system - "automatic" must add at least one real driver on a build
  // that has SDL/XInput compiled in.
  {
    xenon::core::XenonSession session;
    xenon::core::SessionConfig config{};
    config.enable_logging = false;
    config.enable_graphics = false;
    config.enable_input = true;
    config.input_drivers = {"Automatic"};

    auto result = session.initialize(config);
    assert(result.success && "Automatic input driver selection should succeed");
    assert(session.input() != nullptr);
    session.shutdown();
  }

  // Test: an explicit "null" input driver is accepted (headless/test mode)
  // even though it adds no real provider.
  {
    xenon::core::XenonSession session;
    xenon::core::SessionConfig config{};
    config.enable_logging = false;
    config.enable_graphics = false;
    config.enable_input = true;
    config.input_drivers = {"null"};

    auto result = session.initialize(config);
    assert(result.success && "Explicit null input driver should be accepted");
    session.shutdown();
  }

  // Test: an unrecognized input driver name must fail initialization.
  {
    xenon::core::XenonSession session;
    xenon::core::SessionConfig config{};
    config.enable_logging = false;
    config.enable_graphics = false;
    config.enable_input = true;
    config.input_drivers = {"not-a-real-driver"};

    auto result = session.initialize(config);
    assert(!result.success && "Unknown input driver must fail, not silently continue");
    assert(session.state() == xenon::core::SessionState::Failed);
  }

  // Test: a guest XamInput* call reaches the InputSystem owned by the active
  // session through the same unified ExportRegistry XAM/Audio exports use -
  // not a separate/disconnected dispatcher (input::xam::guest historically
  // only targeted cpu::ExternalCallRegistry, which XenonSession never
  // populated, so this ordinal was unreachable from recompiled guest code).
  {
    xenon::core::XenonSession session;
    xenon::core::SessionConfig config{};
    config.enable_logging = false;
    config.enable_graphics = false;
    config.enable_input = true;
    config.input_drivers = {"null"};

    assert(session.initialize(config).success);
    assert(session.exports()->contains("xam", 0x0190u) &&
           "XamInputGetCapabilities should be registered under the xam library");

    xenon::cpu::CpuState state{};
    state.gpr[3] = 0;              // user index
    state.gpr[4] = 0;              // flags
    state.gpr[5] = 0;              // out_caps guest pointer (null driver never writes it)
    xenon::core::ExportCallContext ctx{state, *session.memory(), 0, 0};
    auto call_result = session.exports()->invoke("xam", 0x0190u, ctx);
    assert(call_result.handled &&
           "XamInputGetCapabilities should be handled by the active session's InputSystem");
    session.shutdown();
  }

  // Test: XamUserGetXUID is reachable through the production ExportRegistry
  // at its REAL Xbox 360 ordinal (0x020A), not the invented 0x0180 earlier
  // Xenon code used. This is a literal-ordinal check deliberately independent
  // of xenon::xam::ordinal::XamUserGetXUID (see tests/xam/
  // xam_export_ordinal_tests.cpp's header comment for why): if the enum, the
  // registration, and a test all silently agreed on the same wrong number,
  // no test using the enum could ever catch it. A real signed-in default
  // user (see UserManager::kDefaultOfflineXuid) makes the XUID write
  // deterministic, so this also proves the guest memory write path is real.
  {
    xenon::core::XenonSession session;
    xenon::core::SessionConfig config{};
    config.enable_logging = false;
    config.enable_graphics = false;
    config.enable_input = false;

    assert(session.initialize(config).success);
    assert(session.exports()->contains("xam", 0x020Au) &&
           "XamUserGetXUID should be registered under its real Xbox 360 ordinal (0x020A)");
    assert(!session.exports()->contains("xam", 0x0180u) &&
           "the old invented XamUserGetXUID ordinal (0x0180) must not resolve to anything");

    xenon::cpu::CpuState state{};
    state.gpr[3] = 0;  // user index 0 (default offline user, always signed in)
    xenon::memory::GuestAddress out_xuid{};
    assert(session.memory()->allocate(8, 8, xenon::memory::kReadWrite, false, out_xuid));
    session.memory()->write64_be(out_xuid, 0xCCCCCCCCCCCCCCCCull);  // sentinel
    state.gpr[4] = out_xuid;
    xenon::core::ExportCallContext ctx{state, *session.memory(), 0, 0};
    auto call_result = session.exports()->invoke("xam", 0x020Au, ctx);
    assert(call_result.handled && "XamUserGetXUID should be handled at its real ordinal");
    assert(state.gpr[3] == 0 && "XamUserGetXUID should report Success for the default user");
    const auto written_xuid = session.memory()->read64_be(out_xuid);
    assert(written_xuid == 0xE000000000000001ull &&
           "XamUserGetXUID must write the real default offline XUID through the corrected ordinal");
    session.shutdown();
  }

  // Test: xboxkrnl file I/O exports (NtCreateFile et al.) are registered
  // into the same unified export registry, reachable through it rather than
  // only through the standalone xbox::ImportRegistry that
  // tests/xbox/xbox_import_tests.cpp exercises in isolation.
  {
    xenon::core::XenonSession session;
    xenon::core::SessionConfig config{};
    config.enable_logging = false;
    config.enable_graphics = false;
    config.enable_input = false;

    assert(session.initialize(config).success);
    assert(session.exports()->contains("xboxkrnl", 0x00D2u) &&
           "NtCreateFile should be registered under the xboxkrnl library");
    assert(session.exports()->contains("xboxkrnl", "NtReadFile"));
    assert(session.exports()->contains("xboxkrnl", "NtWriteFile"));
    session.shutdown();
  }

  // Test 2: Export registry
  {
    xenon::core::ExportRegistry registry;
    
    bool called = false;
    xenon::core::ExportDescriptor desc{};
    desc.library = "test";
    desc.name = "TestFunction";
    desc.ordinal = 42;
    desc.handler = [&called](xenon::core::ExportCallContext& ctx) -> bool {
      called = true;
      ctx.cpu.gpr[3] = 0xDEADBEEF;
      return true;
    };
    desc.requirement = xenon::core::ExportRequirement::Required;
    
    assert(registry.register_export(std::move(desc)) && "Export registration should succeed");
    assert(registry.contains("test", 42) && "Export should be registered");
    assert(registry.contains("test", "TestFunction") && "Export should be found by name");
    
    // Test invocation
    xenon::cpu::CpuState state{};
    xenon::cpu::FlatMemory memory(1024 * 1024);
    xenon::core::ExportCallContext ctx{state, memory, 0, 0};
    
    auto result = registry.invoke("test", 42, ctx);
    assert(result.handled && "Export should be handled");
    assert(called && "Export handler should be called");
    assert(state.gpr[3] == 0xDEADBEEF && "Return value should be set");
  }

  // Test 3: Call bridge
  {
    xenon::cpu::CpuState state{};
    state.gpr[3] = 0x12345678;  // arg 0
    state.gpr[4] = 0xABCDEF00;  // arg 1
    
    xenon::cpu::FlatMemory memory(1024 * 1024);
    xenon::core::CallBridge bridge(state, memory);
    
    auto arg0 = bridge.read_u32(0);
    auto arg1 = bridge.read_u32(1);
    
    assert(arg0.has_value() && "Argument 0 should be readable");
    assert(arg1.has_value() && "Argument 1 should be readable");
    assert(*arg0 == 0x12345678 && "Argument 0 value");
    assert(*arg1 == 0xABCDEF00 && "Argument 1 value");
    
    bridge.set_u32_result(0xCAFEBABE);
    assert(state.gpr[3] == 0xCAFEBABE && "Result should be set in r3");
  }

  std::cout << "All tests passed!\n";
  return 0;
}
