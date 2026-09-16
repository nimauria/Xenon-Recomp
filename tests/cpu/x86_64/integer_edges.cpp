#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>

#include "xenon/cpu/flat_memory.hpp"
#include "xenon/cpu/runtime.hpp"

using namespace xenon::cpu;
#define FN(n) ExecutionResult n(CpuState&, MemoryPort&, RuntimeServices&)
FN(edge_addc); FN(edge_adde); FN(edge_addme); FN(edge_addze);
FN(edge_subfc); FN(edge_subfe); FN(edge_neg_o); FN(edge_divd_o);
FN(edge_srawi); FN(edge_sradi); FN(edge_cmpw); FN(edge_cmpld);
#undef FN

int main() {
  FlatMemory memory(0x1000u, 0x1000u);
  NullRuntimeServices runtime;
  CpuState state{};

  state.gpr[3] = std::numeric_limits<std::uint64_t>::max();
  state.gpr[4] = 1;
  edge_addc(state, memory, runtime);
  assert(state.gpr[5] == 0);
  assert(state.xer_ca());

  state.xer = xer_bits::CA;
  state.gpr[3] = std::numeric_limits<std::uint64_t>::max();
  state.gpr[4] = 0;
  edge_adde(state, memory, runtime);
  assert(state.gpr[5] == 0);
  assert(state.xer_ca());

  state.xer = xer_bits::CA;
  state.gpr[3] = 0;
  edge_addme(state, memory, runtime);
  assert(state.gpr[5] == 0);
  assert(state.xer_ca());

  state.xer = xer_bits::CA;
  state.gpr[3] = std::numeric_limits<std::uint64_t>::max();
  edge_addze(state, memory, runtime);
  assert(state.gpr[5] == 0);
  assert(state.xer_ca());

  state.xer = 0;
  state.gpr[3] = 5;
  state.gpr[4] = 5;
  edge_subfc(state, memory, runtime);
  assert(state.gpr[5] == 0 && state.xer_ca());
  state.gpr[3] = 6;
  state.gpr[4] = 5;
  edge_subfc(state, memory, runtime);
  assert(state.gpr[5] == std::numeric_limits<std::uint64_t>::max());
  assert(!state.xer_ca());

  state.xer = xer_bits::CA;
  state.gpr[3] = 5;
  state.gpr[4] = 5;
  edge_subfe(state, memory, runtime);
  assert(state.gpr[5] == 0 && state.xer_ca());
  state.xer = 0;
  edge_subfe(state, memory, runtime);
  assert(state.gpr[5] == std::numeric_limits<std::uint64_t>::max());
  assert(!state.xer_ca());

  state.xer = 0;
  state.gpr[3] = 0x8000000000000000ull;
  edge_neg_o(state, memory, runtime);
  assert(state.gpr[5] == 0x8000000000000000ull);
  assert(state.xer_ov() && state.xer_so());

  state.xer = 0;
  state.gpr[3] = 123;
  state.gpr[4] = 0;
  edge_divd_o(state, memory, runtime);
  assert(state.xer_ov() && state.xer_so());

  state.xer = 0;
  state.gpr[3] = static_cast<std::uint64_t>(static_cast<std::int64_t>(-3));
  edge_srawi(state, memory, runtime);
  assert(state.gpr[5] == static_cast<std::uint64_t>(static_cast<std::int64_t>(-2)));
  assert(state.xer_ca());
  state.xer = 0;
  state.gpr[3] = static_cast<std::uint64_t>(static_cast<std::int64_t>(-4));
  edge_srawi(state, memory, runtime);
  assert(!state.xer_ca());

  state.xer = 0;
  state.gpr[3] = 0xFFFFFFFFFFFFFFFDull;
  edge_sradi(state, memory, runtime);
  assert(state.gpr[5] == 0xFFFFFFFFFFFFFFFEull);
  assert(state.xer_ca());

  state.xer = xer_bits::SO;
  state.gpr[3] = 0x00000000FFFFFFFFull;  // word -1
  state.gpr[4] = 1;
  edge_cmpw(state, memory, runtime);
  assert(state.cr_field(2) == 0x9u);  // LT | SO

  state.xer = 0;
  state.gpr[3] = std::numeric_limits<std::uint64_t>::max();
  state.gpr[4] = 1;
  edge_cmpld(state, memory, runtime);
  assert(state.cr_field(2) == 0x4u);  // unsigned GT

  std::cout << "xenon_cpu_integer_edges: ok\n";
  return 0;
}
