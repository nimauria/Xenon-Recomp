#pragma once

// Guest call tracing for investigations, off unless configured.
//
//   XENON_CALL_TRACE=<hex>[@<reg>:<bytes>]...[,<hex>...]
//     Routes calls to each listed guest function (at most kMaxTracedFunctions)
//     through a logging trampoline. Every traced call writes its arguments
//     (r3-r10, f1-f4, lr, the r13 thread block) on entry and r3/f1 on return
//     to call_trace.log, each stamped t= host milliseconds on the probe logs'
//     steady clock. "@r4:64" also dumps 64 bytes at r4 as big-endian
//     words and floats, once on entry and once on return; several dumps may
//     follow one address ("823AD0D8@r3:32@r4:256"), and "@A2700180:64"
//     dumps a fixed guest address.
//   XENON_CALL_TRACE_LIMIT  records logged per function (default 64); later
//     calls are counted, and every XENON_CALL_TRACE_EVERY-th (default 1000)
//     is still logged.
//   XENON_CALL_GRAPH=1  counts every guest call edge (call site -> target,
//     direct or indirect) per guest thread and writes call_graph.log from
//     write_report().
//   XENON_INTERPRET_RANGE=<hex>-<hex>[,...]  makes the compiled lookup miss
//     for every target inside the inclusive ranges, so those functions run
//     in the dynamic-fallback interpreter: an A/B test of compiled code
//     against the interpreter for one part of a title.
//
// Only calls that go through the compiled lookup are seen: a call the
// generator emitted as a direct C++ call to a function in the same module
// bypasses tracing, the graph and interpretation ranges.
//
// Output goes to XENON_CALL_TRACE_DIR, or the working directory.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

#include "xenon/cpu/runtime.hpp"

namespace xenon::cpu::call_trace {

inline constexpr std::size_t kMaxTracedFunctions = 16;

struct Settings {
  std::string_view functions;  // XENON_CALL_TRACE syntax.
  std::uint32_t limit = 64;
  std::uint32_t every = 1000;
  bool graph = false;
  std::string_view interpret;  // XENON_INTERPRET_RANGE syntax.
  std::filesystem::path directory;
};

// Replaces the configuration (the environment is read once, before the
// first bind, unless configure() ran first). Returns false, configuring
// nothing, when `functions` or `interpret` does not parse.
bool configure(const Settings& settings);

[[nodiscard]] bool active() noexcept;

// Wraps the context's compiled lookup when tracing is active. Call after
// the compiled registry is bound.
void bind(ExecutionContext& context) noexcept;

// Closes call_trace.log (later records append to it) and writes
// call_graph.log.
void write_report();

// Calls of a traced function seen so far (0 for an untraced address).
[[nodiscard]] std::uint64_t call_count(GuestAddress function) noexcept;

}  // namespace xenon::cpu::call_trace
