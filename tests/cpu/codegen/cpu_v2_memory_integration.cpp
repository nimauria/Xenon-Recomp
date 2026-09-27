#include <cassert>
#include <iostream>
#include <string>

#include "xenon/cpu/backend/cpp_aot.hpp"
#include "xenon/cpu/ir.hpp"

using namespace xenon::cpu;
using namespace xenon::cpu::ir;

namespace {

void require_contains(const std::string& source, const std::string& text) {
  if (source.find(text) == std::string::npos) {
    std::cerr << "missing generated fragment: " << text << '\n';
    std::abort();
  }
}

void require_absent(const std::string& source, const std::string& text) {
  if (source.find(text) != std::string::npos) {
    std::cerr << "unexpected legacy generated fragment: " << text << '\n';
    std::abort();
  }
}

}  // namespace

int main() {
  Block block{};
  block.guest_address = 0x82000000u;
  block.end_address = 0x8200001Cu;
  Builder b(block);

  const auto address = b.constant_i64(0x1000u);
  const auto value = b.constant_i32(0x11223344u);
  const ValueId address_args[] = {address};
  const ValueId store_args[] = {address, value};

  const auto loaded = b.emit(Op::Load, Type::I32, address_args,
                             static_cast<std::uint64_t>(Endian::Big));
  b.write_gpr(3, loaded);
  b.emit(Op::Store, Type::Void, store_args,
         static_cast<std::uint64_t>(Endian::Big),
         static_cast<std::uint64_t>(Type::I32));
  (void)b.emit(Op::ReserveLoad, Type::I32, address_args);
  (void)b.emit(Op::StoreConditional, Type::I1, store_args, 0,
               static_cast<std::uint64_t>(Type::I32));
  b.emit(Op::Barrier, Type::Void, {}, 1u);
  b.emit(Op::ICacheInvalidate, Type::Void, address_args);

  const auto source = backend::CppAotBackend{}.emit_function(block, "memory_v2_path");

  require_contains(source, "memory_v2_path_v2([[maybe_unused]] ExecutionContext& context)");
  require_contains(source, "auto& memory_access = context.memory_access;");
  require_contains(source, "memory_access.read32_be(");
  require_contains(source, "memory_access.write32_be(");
  require_contains(source, "memory_access.reserve32(");
  require_contains(source, "memory_access.store_conditional32(");
  require_contains(source, "memory_access.barrier(BarrierKind::Sync)");
  require_contains(source, "memory_access.instruction_cache_invalidate(");
  require_contains(source, "ExecutionContext context(state, memory, runtime);");
  require_contains(source, "return memory_v2_path_v2(context);");

  require_absent(source, "memory.read32_be(");
  require_absent(source, "memory.write32_be(");
  require_absent(source, "memory.reserve32(");
  require_absent(source, "memory.store_conditional32(");
  require_absent(source, "memory.barrier(");

  std::cout << "xenon_cpu_v2_memory_integration: ok\n";
  return 0;
}
