// Guest call tracing (xenon/cpu/call_trace.hpp): traced calls run the
// original code through a trampoline and log their registers and buffers;
// the call graph counts direct and indirect call edges per guest thread.
#include <bit>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "xenon/cpu/call_trace.hpp"
#include "xenon/memory/address_space.hpp"

using namespace xenon::cpu;

namespace {

constexpr GuestAddress kTraced = 0x82000100u;
constexpr GuestAddress kUntraced = 0x82000200u;
constexpr GuestAddress kBuffer = 0x00100000u;

class NullRuntime final : public RuntimeServices {
 public:
  ExecutionResult call(GuestAddress, CpuState&, MemoryPort&) override { return {FlowReason::Trap, 0, 0}; }
  ExecutionResult syscall(std::uint32_t, CpuState&, MemoryPort&) override { return {FlowReason::Trap, 0, 0}; }
  ExecutionResult trap(std::uint32_t, CpuState&, MemoryPort&) override { return {FlowReason::Trap, 0, 0}; }
  std::uint64_t read_spr(std::uint32_t, const CpuState&) override { return 0; }
  void write_spr(std::uint32_t, std::uint64_t, CpuState&) override {}
  std::uint64_t read_time_base(const CpuState&) override { return 0; }
};

int g_traced_runs = 0;

// Scales the float at r4 by f1, returns r3 + 1.
ExecutionResult traced_function(ExecutionContext& context) {
  ++g_traced_runs;
  const auto address = static_cast<GuestAddress>(context.state.gpr[4]);
  const float value = std::bit_cast<float>(context.memory.read32_be(address));
  context.memory.write32_be(
      address, std::bit_cast<std::uint32_t>(value * static_cast<float>(context.state.fpr(1))));
  context.state.gpr[3] += 1u;
  return {FlowReason::Return, static_cast<GuestAddress>(context.state.lr), 0};
}

ExecutionResult untraced_function(ExecutionContext& context) {
  return {FlowReason::Return, static_cast<GuestAddress>(context.state.lr), 0};
}

int g_registry_tag = 0;

NativeCompiledEntry module_lookup(void* registry, ExecutionContext&, GuestAddress target,
                                  CompiledLookupKind) {
  assert(registry == &g_registry_tag);  // The wrapped lookup keeps its registry.
  if (target == kTraced) return &traced_function;
  if (target == kUntraced) return &untraced_function;
  return nullptr;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// Calls `target` the way generated code does: call site in cia, then lookup.
ExecutionResult call(ExecutionContext& context, GuestAddress call_site, GuestAddress target) {
  context.state.cia = call_site;
  context.state.lr = call_site + 4u;
  auto* entry = context.lookup_compiled(target, CompiledLookupKind::Call);
  assert(entry);
  return entry(context);
}

}  // namespace

int main() {
  const auto directory = std::filesystem::temp_directory_path() / "xenon_call_trace_tests";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);

  // Malformed specifications configure nothing.
  call_trace::Settings bad;
  bad.functions = "8200010G";
  assert(!call_trace::configure(bad));
  bad.functions = "82000102";  // Not an instruction address.
  assert(!call_trace::configure(bad));
  bad.functions = "82000100@r4";
  assert(!call_trace::configure(bad));
  bad.functions = "82000100@r40:16";
  assert(!call_trace::configure(bad));
  bad.functions = "82000100@100002:16";  // A fixed dump address is word aligned.
  assert(!call_trace::configure(bad));

  call_trace::Settings settings;
  settings.functions = "0x82000100@r4:16@100004:4";
  settings.limit = 2;
  settings.every = 3;
  settings.graph = true;
  settings.directory = directory;
  assert(call_trace::configure(settings));
  assert(call_trace::active());

  xenon::memory::AddressSpace memory;
  assert(memory.initialize());
  assert(memory.commit_fixed(kBuffer, 0x1000u, xenon::memory::kReadWrite));
  memory.write32_be(kBuffer, std::bit_cast<std::uint32_t>(0.5f));

  NullRuntime runtime;
  CpuState state{};
  state.gpr[13] = 0x7000C000u;
  ExecutionContext context(state, memory, runtime);
  context.compiled_registry = &g_registry_tag;
  context.compiled_lookup = &module_lookup;
  call_trace::bind(context);
  call_trace::bind(context);  // Binding twice does not wrap twice.
  assert(context.compiled_registry != &g_registry_tag);

  // A traced call runs the original code with the caller's registers and
  // returns its result unchanged.
  for (int i = 0; i < 7; ++i) {
    state.gpr[3] = 10u;
    state.gpr[4] = kBuffer;
    state.set_fpr(1, 2.0);
    const auto result = call(context, 0x82001000u, kTraced);
    assert(result.reason == FlowReason::Return && result.next_address == 0x82001004u);
    assert(state.gpr[3] == 11u);
  }
  assert(g_traced_runs == 7);
  assert(call_trace::call_count(kTraced) == 7u);
  assert(std::bit_cast<float>(memory.read32_be(kBuffer)) == 64.0f);

  // Untraced functions keep their own entry: no trampoline, no count.
  assert(context.lookup_compiled(kUntraced, CompiledLookupKind::Branch) == &untraced_function);
  call(context, 0x82001010u, kUntraced);
  call(context, 0x82001010u, kUntraced);
  assert(call_trace::call_count(kUntraced) == 0u);
  assert(context.lookup_compiled(0x82000300u, CompiledLookupKind::Branch) == nullptr);

  call_trace::write_report();

  // Calls 1 and 2 (the limit), then every 3rd: call 5. Each with its entry
  // registers, the buffer before and after, and the return.
  const auto trace = read_file(directory / "call_trace.log");
  assert(trace.find("-> 82000100 #1 lr=82001004 r13=7000C000 t=") != std::string::npos);
  assert(trace.find("r3=0000000A r4=00100000") != std::string::npos);
  assert(trace.find("f1=2 ") != std::string::npos);
  assert(trace.find("<- 82000100 #1 Return next=82001004 r3=0000000B") != std::string::npos);
  assert(trace.find("-> 82000100 #2 ") != std::string::npos);
  assert(trace.find("-> 82000100 #3 ") == std::string::npos);
  assert(trace.find("-> 82000100 #4 ") == std::string::npos);
  assert(trace.find("-> 82000100 #5 ") != std::string::npos);
  assert(trace.find("-> 82000100 #6 ") == std::string::npos);
  assert(trace.find("r4=00100000, 16 bytes:") != std::string::npos);
  assert(trace.find("+000: 3F000000 00000000 00000000 00000000  | 0.5 0 0 0") != std::string::npos);
  assert(trace.find("+000: 3F800000 00000000") != std::string::npos);  // After call 1.
  assert(trace.find("    00100004, 4 bytes:\n      +000: 00000000  | 0\n") != std::string::npos);
  assert(trace.find("82000200") == std::string::npos);

  // The graph counts every call edge, traced or not, by call site.
  const auto graph = read_file(directory / "call_graph.log");
  assert(graph.find("== thread r13=7000C000: 2 edges, 9 calls") != std::string::npos);
  assert(graph.find("           7  82001000 -> 82000100") != std::string::npos);
  assert(graph.find("           2  82001010 -> 82000200") != std::string::npos);

  // An unmapped dump address is shown as unreadable, not read.
  // Reconfiguring starts a fresh log and fresh counts.
  settings.functions = "82000200@r4:8";
  settings.graph = false;
  assert(call_trace::configure(settings));
  assert(call_trace::call_count(kTraced) == 0u);
  {
    CpuState other{};
    other.gpr[4] = 0x30000000u;  // Never committed.
    ExecutionContext probe(other, memory, runtime);
    probe.compiled_registry = &g_registry_tag;
    probe.compiled_lookup = &module_lookup;
    call_trace::bind(probe);
    assert(probe.lookup_compiled(kTraced, CompiledLookupKind::Call) == &traced_function);
    call(probe, 0x82001020u, kUntraced);
  }
  call_trace::write_report();
  const auto unmapped = read_file(directory / "call_trace.log");
  assert(unmapped.find("82000100") == std::string::npos);
  assert(unmapped.find("-> 82000200 #1 ") != std::string::npos);
  assert(unmapped.find("+000: -------- --------  | ? ?") != std::string::npos);
  assert(!std::filesystem::exists(directory / "call_graph.log") ||
         read_file(directory / "call_graph.log") == graph);

  // Interpretation ranges make the compiled lookup miss inside them only.
  call_trace::Settings ranges;
  ranges.interpret = "82000200-820002FC";
  ranges.directory = directory;
  assert(call_trace::configure(ranges));
  assert(call_trace::active());
  {
    CpuState other{};
    ExecutionContext probe(other, memory, runtime);
    probe.compiled_registry = &g_registry_tag;
    probe.compiled_lookup = &module_lookup;
    call_trace::bind(probe);
    assert(probe.lookup_compiled(kUntraced, CompiledLookupKind::Call) == nullptr);
    assert(probe.lookup_compiled(kUntraced, CompiledLookupKind::Branch) == nullptr);
    assert(probe.lookup_compiled(kTraced, CompiledLookupKind::Call) == &traced_function);
  }
  ranges.interpret = "82000300-82000200";  // Reversed.
  assert(!call_trace::configure(ranges));
  ranges.interpret = "82000200";
  assert(!call_trace::configure(ranges));

  std::filesystem::remove_all(directory);
  std::cout << "call trace tests passed\n";
  return 0;
}
