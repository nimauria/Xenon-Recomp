#include "xenon/cpu/call_trace.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xenon::cpu::call_trace {
namespace {

constexpr std::uint32_t kMaxDumpBytes = 4096u;
constexpr std::size_t kGraphEdgesPerThread = 4000u;

// Bytes at a register's value, or at a fixed guest address.
struct Dump {
  std::uint8_t reg{};
  bool fixed{};
  GuestAddress address{};
  std::uint32_t bytes{};
};

struct Traced {
  GuestAddress address{};
  std::vector<Dump> dumps;
  // The entry the trampoline forwards to: the first one a lookup returned.
  std::atomic<NativeCompiledEntry> original{};
  std::atomic<std::uint64_t> calls{};
};

// The lookup a context had before tracing wrapped it.
struct Binding {
  CompiledLookupCallback inner{};
  void* registry{};
};

// Call edges seen by one host thread, keyed (call site << 32 | target).
struct ThreadGraph {
  std::mutex mutex;
  std::uint64_t guest_thread_block{};  // r13 when first seen.
  std::unordered_map<std::uint64_t, std::uint64_t> edges;
};

struct TraceState {
  std::mutex mutex;  // Bindings, graphs, the trace file.
  std::array<Traced, kMaxTracedFunctions> traced;
  std::size_t traced_count{};
  std::uint32_t limit{64};
  std::uint32_t every{1000};
  bool graph{};
  std::vector<std::pair<GuestAddress, GuestAddress>> interpret;  // Inclusive.
  std::filesystem::path directory;
  std::vector<std::unique_ptr<Binding>> bindings;
  std::vector<std::shared_ptr<ThreadGraph>> graphs;
  std::atomic<std::uint64_t> generation{};
  FILE* trace_file{};
  bool trace_started{};  // Later opens append to this configuration's log.
};

TraceState& state() {
  static TraceState instance;
  return instance;
}

std::atomic<bool> g_active{false};
std::atomic<bool> g_configured{false};
std::once_flag g_environment_once;

[[nodiscard]] bool parse_number(std::string_view text, int base, std::uint64_t& out) {
  if (text.empty()) return false;
  if (base == 0 && text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    text.remove_prefix(2);
    base = 16;
  } else if (base == 0) {
    base = 10;
  } else if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
    text.remove_prefix(2);
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    int digit = -1;
    if (c >= '0' && c <= '9') digit = c - '0';
    else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
    if (digit < 0 || digit >= base) return false;
    value = value * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(digit);
    if (value > 0xFFFFFFFFull) return false;
  }
  out = value;
  return true;
}

struct ParsedFunction {
  GuestAddress address{};
  std::vector<Dump> dumps;
};

[[nodiscard]] bool parse_functions(std::string_view spec, std::vector<ParsedFunction>& out) {
  out.clear();
  while (!spec.empty()) {
    const auto comma = spec.find(',');
    auto item = spec.substr(0, comma);
    spec = comma == std::string_view::npos ? std::string_view{} : spec.substr(comma + 1);
    if (item.empty()) continue;
    ParsedFunction function;
    const auto at = item.find('@');
    std::uint64_t address = 0;
    if (!parse_number(item.substr(0, at), 16, address) || (address & 3u) != 0u) return false;
    function.address = static_cast<GuestAddress>(address);
    auto rest = at == std::string_view::npos ? std::string_view{} : item.substr(at);
    while (!rest.empty()) {
      // "@r<reg>:<bytes>" or "@<hex address>:<bytes>"
      if (rest.size() < 2 || rest[0] != '@') return false;
      rest.remove_prefix(1);
      const bool fixed = rest[0] != 'r' && rest[0] != 'R';
      if (!fixed) rest.remove_prefix(1);
      const auto colon = rest.find(':');
      if (colon == std::string_view::npos) return false;
      const auto next = rest.find('@', colon);
      std::uint64_t where = 0;
      std::uint64_t bytes = 0;
      if (fixed ? !parse_number(rest.substr(0, colon), 16, where) || (where & 3u) != 0u
                : !parse_number(rest.substr(0, colon), 10, where) || where > 31u) {
        return false;
      }
      if (!parse_number(rest.substr(colon + 1, next == std::string_view::npos
                                                   ? std::string_view::npos
                                                   : next - colon - 1),
                        0, bytes) ||
          bytes == 0u || bytes > kMaxDumpBytes) {
        return false;
      }
      Dump dump;
      dump.fixed = fixed;
      if (fixed) dump.address = static_cast<GuestAddress>(where);
      else dump.reg = static_cast<std::uint8_t>(where);
      dump.bytes = static_cast<std::uint32_t>((bytes + 3u) & ~3ull);
      function.dumps.push_back(dump);
      rest = next == std::string_view::npos ? std::string_view{} : rest.substr(next);
    }
    out.push_back(std::move(function));
  }
  return out.size() <= kMaxTracedFunctions;
}

[[nodiscard]] bool parse_ranges(std::string_view spec,
                                std::vector<std::pair<GuestAddress, GuestAddress>>& out) {
  out.clear();
  while (!spec.empty()) {
    const auto comma = spec.find(',');
    const auto item = spec.substr(0, comma);
    spec = comma == std::string_view::npos ? std::string_view{} : spec.substr(comma + 1);
    if (item.empty()) continue;
    const auto dash = item.find('-');
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    if (dash == std::string_view::npos || !parse_number(item.substr(0, dash), 16, first) ||
        !parse_number(item.substr(dash + 1), 16, last) || last < first) {
      return false;
    }
    out.emplace_back(static_cast<GuestAddress>(first), static_cast<GuestAddress>(last));
  }
  return true;
}

void close_trace_file_locked(TraceState& s) {
  if (s.trace_file) std::fclose(s.trace_file);
  s.trace_file = nullptr;
}

void write_record(const std::string& text) {
  auto& s = state();
  std::scoped_lock lock(s.mutex);
  if (!s.trace_file) {
    const auto path = s.directory.empty() ? std::filesystem::path("call_trace.log")
                                          : s.directory / "call_trace.log";
    s.trace_file = std::fopen(path.string().c_str(), s.trace_started ? "ab" : "wb");
    s.trace_started = true;
    if (!s.trace_file) return;
  }
  std::fwrite(text.data(), 1, text.size(), s.trace_file);
  std::fflush(s.trace_file);
}

void append_format(std::string& out, const char* format, ...) {
  char buffer[512];
  va_list arguments;
  va_start(arguments, format);
  const int written = std::vsnprintf(buffer, sizeof(buffer), format, arguments);
  va_end(arguments);
  if (written > 0)
    out.append(buffer, std::min<std::size_t>(static_cast<std::size_t>(written), sizeof(buffer) - 1));
}

void append_dump(std::string& out, ExecutionContext& context, const Dump& dump,
                 const std::array<std::uint64_t, 32>& gpr) {
  const auto base = dump.fixed ? dump.address : static_cast<GuestAddress>(gpr[dump.reg]);
  const auto bytes = dump.bytes;
  if (dump.fixed) append_format(out, "    %08X, %u bytes:\n", base, bytes);
  else append_format(out, "    r%u=%08X, %u bytes:\n", static_cast<unsigned>(dump.reg), base, bytes);
  for (std::uint32_t offset = 0; offset < bytes; offset += 16u) {
    append_format(out, "      +%03X:", offset);
    std::string floats;
    for (std::uint32_t word = offset; word < std::min(bytes, offset + 16u); word += 4u) {
      MemoryAccessContext::PhysicalResolution resolved{};
      if (!context.memory_access.resolve_physical_ram(base + word, 4u, false, 4u, resolved)) {
        out += " --------";
        floats += " ?";
        continue;
      }
      std::uint32_t raw{};
      std::memcpy(&raw, resolved.ptr, sizeof(raw));
      if constexpr (std::endian::native == std::endian::little)
        raw = (raw >> 24u) | ((raw >> 8u) & 0xFF00u) | ((raw << 8u) & 0xFF0000u) | (raw << 24u);
      append_format(out, " %08X", raw);
      append_format(floats, " %.6g", static_cast<double>(std::bit_cast<float>(raw)));
    }
    out += "  |";
    out += floats;
    out += '\n';
  }
}

// The probe logs' clock, so calls line up with their records.
long long now_milliseconds() noexcept {
  return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
}

void log_entry(const Traced& traced, std::uint64_t call, ExecutionContext& context) {
  const auto& cpu = context.state;
  std::string out;
  append_format(out,
                "-> %08X #%llu lr=%08X r13=%08X t=%lld\n"
                "   r3=%08llX r4=%08llX r5=%08llX r6=%08llX r7=%08llX r8=%08llX r9=%08llX r10=%08llX\n"
                "   f1=%.9g f2=%.9g f3=%.9g f4=%.9g\n",
                traced.address, static_cast<unsigned long long>(call),
                static_cast<unsigned>(cpu.lr), static_cast<unsigned>(cpu.gpr[13]),
                now_milliseconds(),
                static_cast<unsigned long long>(cpu.gpr[3]), static_cast<unsigned long long>(cpu.gpr[4]),
                static_cast<unsigned long long>(cpu.gpr[5]), static_cast<unsigned long long>(cpu.gpr[6]),
                static_cast<unsigned long long>(cpu.gpr[7]), static_cast<unsigned long long>(cpu.gpr[8]),
                static_cast<unsigned long long>(cpu.gpr[9]), static_cast<unsigned long long>(cpu.gpr[10]),
                cpu.fpr(1), cpu.fpr(2), cpu.fpr(3), cpu.fpr(4));
  for (const auto& dump : traced.dumps)
    append_dump(out, context, dump, cpu.gpr);
  write_record(out);
}

void log_exit(const Traced& traced, std::uint64_t call, ExecutionContext& context,
              const std::array<std::uint64_t, 32>& entry_gpr,
              const ExecutionResult& result) {
  const auto& cpu = context.state;
  std::string out;
  append_format(out, "<- %08X #%llu %s next=%08llX r3=%08llX f1=%.9g t=%lld\n", traced.address,
                static_cast<unsigned long long>(call),
                std::string(flow_reason_name(result.reason)).c_str(),
                static_cast<unsigned long long>(result.next_address),
                static_cast<unsigned long long>(cpu.gpr[3]), cpu.fpr(1), now_milliseconds());
  // Dumps on return read the buffers the call was given.
  for (const auto& dump : traced.dumps)
    append_dump(out, context, dump, entry_gpr);
  write_record(out);
}

template <std::size_t Index>
ExecutionResult trampoline(ExecutionContext& context) {
  auto& s = state();
  auto& traced = s.traced[Index];
  auto* original = traced.original.load(std::memory_order_acquire);
  const auto call = traced.calls.fetch_add(1u, std::memory_order_relaxed) + 1u;
  const bool logged = call <= s.limit || (s.every && (call - s.limit) % s.every == 0u);
  if (!logged) return original(context);
  const auto entry_gpr = context.state.gpr;
  log_entry(traced, call, context);
  const auto result = original(context);
  log_exit(traced, call, context, entry_gpr, result);
  return result;
}

template <std::size_t... Index>
constexpr std::array<NativeCompiledEntry, sizeof...(Index)> make_trampolines(
    std::index_sequence<Index...>) {
  return {&trampoline<Index>...};
}

constexpr auto kTrampolines =
    make_trampolines(std::make_index_sequence<kMaxTracedFunctions>{});

struct ThreadGraphSlot {
  std::uint64_t generation{~0ull};
  std::shared_ptr<ThreadGraph> graph;
};
thread_local ThreadGraphSlot t_graph;

void record_edge(ExecutionContext& context, GuestAddress target) {
  auto& s = state();
  const auto generation = s.generation.load(std::memory_order_acquire);
  if (t_graph.generation != generation || !t_graph.graph) {
    auto graph = std::make_shared<ThreadGraph>();
    graph->guest_thread_block = context.state.gpr[13];
    {
      std::scoped_lock lock(s.mutex);
      s.graphs.push_back(graph);
    }
    t_graph = {generation, std::move(graph)};
  }
  const auto key = (static_cast<std::uint64_t>(context.state.cia) << 32u) | target;
  std::scoped_lock lock(t_graph.graph->mutex);
  ++t_graph.graph->edges[key];
}

NativeCompiledEntry traced_lookup(void* registry, ExecutionContext& context,
                                  GuestAddress target, CompiledLookupKind kind) {
  const auto* binding = static_cast<const Binding*>(registry);
  auto* entry = binding->inner ? binding->inner(binding->registry, context, target, kind)
                               : nullptr;
  auto& s = state();
  if (s.graph && kind == CompiledLookupKind::Call) record_edge(context, target);
  if (!entry) return nullptr;
  for (const auto& [first, last] : s.interpret)
    if (target >= first && target <= last) return nullptr;
  for (std::size_t i = 0; i < s.traced_count; ++i) {
    auto& traced = s.traced[i];
    if (traced.address != target) continue;
    NativeCompiledEntry expected = nullptr;
    if (traced.original.compare_exchange_strong(expected, entry, std::memory_order_acq_rel) ||
        expected == entry) {
      return kTrampolines[i];
    }
    // Another registry compiled this address differently: run it untraced.
    return entry;
  }
  return entry;
}

void configure_from_environment() {
  if (g_configured.load()) return;
  const char* functions = std::getenv("XENON_CALL_TRACE");
  const char* graph = std::getenv("XENON_CALL_GRAPH");
  const char* interpret = std::getenv("XENON_INTERPRET_RANGE");
  const bool graph_on = graph && *graph && std::string_view(graph) != "0";
  if (!(functions && *functions) && !graph_on && !(interpret && *interpret)) return;
  Settings settings;
  settings.functions = functions ? functions : "";
  settings.graph = graph_on;
  settings.interpret = interpret ? interpret : "";
  std::uint64_t value = 0;
  if (const char* limit = std::getenv("XENON_CALL_TRACE_LIMIT");
      limit && parse_number(limit, 10, value)) {
    settings.limit = static_cast<std::uint32_t>(value);
  }
  if (const char* every = std::getenv("XENON_CALL_TRACE_EVERY");
      every && parse_number(every, 10, value)) {
    settings.every = static_cast<std::uint32_t>(value);
  }
  if (const char* directory = std::getenv("XENON_CALL_TRACE_DIR"); directory && *directory)
    settings.directory = directory;
  if (!configure(settings)) {
    std::fprintf(stderr, "[call_trace] XENON_CALL_TRACE does not parse: %s\n", functions);
  }
}

}  // namespace

bool configure(const Settings& settings) {
  std::vector<ParsedFunction> functions;
  std::vector<std::pair<GuestAddress, GuestAddress>> interpret;
  if (!parse_functions(settings.functions, functions) ||
      !parse_ranges(settings.interpret, interpret)) {
    return false;
  }
  auto& s = state();
  {
    std::scoped_lock lock(s.mutex);
    close_trace_file_locked(s);
    s.trace_started = false;
    for (std::size_t i = 0; i < kMaxTracedFunctions; ++i) {
      auto& traced = s.traced[i];
      traced.address = i < functions.size() ? functions[i].address : 0u;
      traced.dumps = i < functions.size() ? functions[i].dumps : std::vector<Dump>{};
      traced.original.store(nullptr);
      traced.calls.store(0u);
    }
    s.traced_count = functions.size();
    s.limit = settings.limit;
    s.every = settings.every;
    s.graph = settings.graph;
    s.interpret = std::move(interpret);
    s.directory = settings.directory;
    s.graphs.clear();
    s.generation.fetch_add(1u, std::memory_order_acq_rel);
  }
  g_configured.store(true);
  g_active.store(!functions.empty() || settings.graph || !s.interpret.empty());
  return true;
}

bool active() noexcept {
  std::call_once(g_environment_once, configure_from_environment);
  return g_active.load(std::memory_order_relaxed);
}

void bind(ExecutionContext& context) noexcept {
  if (!active() || context.compiled_lookup == &traced_lookup) return;
  auto& s = state();
  std::scoped_lock lock(s.mutex);
  Binding* binding = nullptr;
  for (const auto& existing : s.bindings) {
    if (existing->inner == context.compiled_lookup &&
        existing->registry == context.compiled_registry) {
      binding = existing.get();
      break;
    }
  }
  if (!binding) {
    s.bindings.push_back(
        std::make_unique<Binding>(Binding{context.compiled_lookup, context.compiled_registry}));
    binding = s.bindings.back().get();
  }
  context.compiled_registry = binding;
  context.compiled_lookup = &traced_lookup;
}

void write_report() {
  auto& s = state();
  std::scoped_lock lock(s.mutex);
  close_trace_file_locked(s);
  if (!s.graph) return;
  const auto path = s.directory.empty() ? std::filesystem::path("call_graph.log")
                                        : s.directory / "call_graph.log";
  FILE* file = std::fopen(path.string().c_str(), "wb");
  if (!file) return;
  for (const auto& graph : s.graphs) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> edges;
    std::uint64_t total = 0;
    {
      std::scoped_lock graph_lock(graph->mutex);
      edges.assign(graph->edges.begin(), graph->edges.end());
    }
    for (const auto& edge : edges) total += edge.second;
    std::sort(edges.begin(), edges.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(file, "== thread r13=%08X: %zu edges, %llu calls\n",
                 static_cast<unsigned>(graph->guest_thread_block), edges.size(),
                 static_cast<unsigned long long>(total));
    const auto shown = std::min(edges.size(), kGraphEdgesPerThread);
    for (std::size_t i = 0; i < shown; ++i) {
      std::fprintf(file, "%12llu  %08X -> %08X\n",
                   static_cast<unsigned long long>(edges[i].second),
                   static_cast<unsigned>(edges[i].first >> 32u),
                   static_cast<unsigned>(edges[i].first & 0xFFFFFFFFu));
    }
  }
  std::fclose(file);
}

std::uint64_t call_count(GuestAddress function) noexcept {
  auto& s = state();
  for (std::size_t i = 0; i < s.traced_count; ++i)
    if (s.traced[i].address == function) return s.traced[i].calls.load(std::memory_order_relaxed);
  return 0u;
}

}  // namespace xenon::cpu::call_trace
