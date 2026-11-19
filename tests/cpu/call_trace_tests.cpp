// Guest call tracing (xenon/cpu/call_trace.hpp): traced calls run the
// original code through a trampoline and log their registers and buffers;
// the call graph counts direct and indirect call edges per guest thread.
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "xenon/cpu/call_trace.hpp"
#include "xenon/cpu/memory_port.hpp"
#include "xenon/logging/probe_log.hpp"
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

  // An indirect dump reads the buffer a descriptor argument points to: here
  // r5 is a descriptor whose +8 holds a pointer. A pointer slot that is not
  // mapped is reported, not followed.
  bad.functions = "82000100@[r5+8]";  // No size.
  assert(!call_trace::configure(bad));
  bad.functions = "82000100@[x5+8]:4";
  assert(!call_trace::configure(bad));
  bad.functions = "82000100@[r40]:4";
  assert(!call_trace::configure(bad));
  bad.functions = "82000100@[r5+8:4";
  assert(!call_trace::configure(bad));
  settings.functions = "82000100@[r5+8]:8@[r6]:4";
  assert(call_trace::configure(settings));
  memory.write32_be(kBuffer + 0x208u, kBuffer + 0x400u);
  memory.write32_be(kBuffer + 0x400u, std::bit_cast<std::uint32_t>(1.5f));
  memory.write32_be(kBuffer + 0x404u, 0xFEFEFEFEu);
  memory.write32_be(kBuffer + 0x300u, std::bit_cast<std::uint32_t>(1.0f));
  {
    CpuState other{};
    other.gpr[4] = kBuffer + 0x300u;
    other.gpr[5] = kBuffer + 0x200u;
    other.gpr[6] = 0x30000000u;  // Never committed.
    ExecutionContext probe(other, memory, runtime);
    probe.compiled_registry = &g_registry_tag;
    probe.compiled_lookup = &module_lookup;
    call_trace::bind(probe);
    call(probe, 0x82001030u, kTraced);
  }
  call_trace::write_report();
  const auto indirect = read_file(directory / "call_trace.log");
  assert(indirect.find("    [r5+8]=00100400, 8 bytes:\n      +000: 3FC00000 FEFEFEFE  | 1.5 ") !=
         std::string::npos);
  assert(indirect.find("    [r6+0] (30000000) is not readable\n") != std::string::npos);

  // A watch arms the write watch, once, on the buffer the first call points
  // to - found only while the title runs - and is not dumped. Stores into it
  // are then reported in write_watch_diag.log beside the probe logs.
  bad.functions = "82000100@watch";
  assert(!call_trace::configure(bad));
  bad.functions = "82000100@watch[r5+8]";  // No size.
  assert(!call_trace::configure(bad));
  bad.functions = "82000100@watch[r5+8]:0x10004";  // Over the 64 KiB a watch guards.
  assert(!call_trace::configure(bad));
  bad.functions = "82000100@[r5+8]:0x10000";  // Over a dump's 4 KiB.
  assert(!call_trace::configure(bad));
  settings.functions = "82000100@watch[r5+8]:0x10000";
  assert(call_trace::configure(settings));
  xenon::logging::set_probe_log_directory(directory.string());
  settings.functions = "82000100@watch[r5+8]:16";
  assert(call_trace::configure(settings));
  const auto watched = memory.get_physical_address(kBuffer + 0x400u);
  for (int i = 0; i < 2; ++i) {
    CpuState other{};
    other.gpr[4] = kBuffer + 0x300u;
    other.gpr[5] = kBuffer + 0x200u;
    ExecutionContext probe(other, memory, runtime);
    probe.compiled_registry = &g_registry_tag;
    probe.compiled_lookup = &module_lookup;
    call_trace::bind(probe);
    call(probe, 0x82001040u, kTraced);
  }
  assert(xenon::cpu::write_watch_hit(watched, 4u));
  assert(!xenon::cpu::write_watch_hit(watched + 16u, 4u));
  memory.write32_be(kBuffer + 0x404u, 0x12345678u);
  call_trace::write_report();
  const auto armed = read_file(directory / "call_trace.log");
  char expected[112];
  std::snprintf(expected, sizeof(expected),
                "== 82000100 #1: write watch armed on 00100400 (physical %08X), 16 bytes\n",
                static_cast<unsigned>(watched));
  assert(armed.find(expected) != std::string::npos);
  assert(armed.find("#2: write watch") == std::string::npos);
  assert(armed.find("[r5+8]=") == std::string::npos);  // Not dumped.
  const auto stores = read_file(directory / "write_watch_diag.log");
  std::snprintf(expected, sizeof(expected), "at guest 0x00100404 (physical 0x%08X), data 12345678",
                static_cast<unsigned>(watched + 4u));
  assert(stores.find(expected) != std::string::npos);  // Readable while open.
  // A report names the guest call stack: the return addresses saved along
  // the storing thread's r1 back chain (LR at -8 from each caller's frame).
  constexpr GuestAddress kStack = 0x00200000u;
  assert(memory.commit_fixed(kStack, 0x1000u, xenon::memory::kReadWrite));
  memory.write32_be(kStack + 0x100u, kStack + 0x200u);  // Back chain.
  memory.write32_be(kStack + 0x1F8u, 0x82001234u);       // Saved LR.
  memory.write32_be(kStack + 0x200u, kStack + 0x300u);
  memory.write32_be(kStack + 0x2F8u, 0x82005678u);
  memory.write32_be(kStack + 0x300u, 0u);  // The outermost frame.
  {
    GuestAddress callers[4]{};
    assert(xenon::cpu::write_watch_guest_backtrace(kStack + 0x100u, callers, 4u) == 2u);
    assert(callers[0] == 0x82001234u && callers[1] == 0x82005678u);
    assert(xenon::cpu::write_watch_guest_backtrace(kStack + 0x100u, callers, 1u) == 1u);
    assert(xenon::cpu::write_watch_guest_backtrace(0x30000000u, callers, 4u) == 0u);
    CpuState storing{};
    storing.gpr[1] = kStack + 0x100u;
    storing.cia = 0x82000110u;
    storing.gpr[27] = 0x40001234u;
    const auto* outer = xenon::cpu::set_write_trap_guest_state(&storing);
    memory.write32_be(kBuffer + 0x408u, 0xCAFEF00Du);
    xenon::cpu::set_write_trap_guest_state(outer);
  }
  const auto with_stack = read_file(directory / "write_watch_diag.log");
  const auto store_at = with_stack.find("data CAFEF00D");
  assert(store_at != std::string::npos);
  assert(with_stack.find("  guest cia=0x82000110", store_at) != std::string::npos);
  assert(with_stack.find("  guest stack: 82001234 82005678\n", store_at) != std::string::npos);
  assert(with_stack.find("  r24 00000000 00000000 00000000 40001234 00000000", store_at) !=
         std::string::npos);  // Every GPR, eight to a row.

  // Without XENON_WRITE_WATCH_REPORTS the default limit applies.
  assert(xenon::cpu::write_watch_report_limit() == xenon::cpu::kMaxWriteWatchReports);

  // The guarded range is kept for the run: where pages are guarded
  // (Windows) a later configuration's watch is not armed.
  assert(call_trace::configure(settings));
  {
    CpuState other{};
    other.gpr[4] = kBuffer + 0x300u;
    other.gpr[5] = kBuffer + 0x200u;
    ExecutionContext probe(other, memory, runtime);
    probe.compiled_registry = &g_registry_tag;
    probe.compiled_lookup = &module_lookup;
    call_trace::bind(probe);
    call(probe, 0x82001040u, kTraced);
  }
  call_trace::write_report();
#if defined(_WIN32)
  assert(read_file(directory / "call_trace.log").find("#1: write watch NOT armed") !=
         std::string::npos);
#else
  assert(read_file(directory / "call_trace.log").find("#1: write watch armed") !=
         std::string::npos);
#endif

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

  std::error_code ignored;  // write_watch_diag.log stays open until exit.
  std::filesystem::remove_all(directory, ignored);
  std::cout << "call trace tests passed\n";
  return 0;
}
