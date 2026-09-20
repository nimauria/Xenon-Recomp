#include "xenon/core/session.hpp"

#include "xenon/core/call_bridge.hpp"
#include "xenon/cpu/flat_memory.hpp"

#include <cassert>
#include <iostream>

int main() {
  std::cout << "Testing Xenon Session...\n";

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
