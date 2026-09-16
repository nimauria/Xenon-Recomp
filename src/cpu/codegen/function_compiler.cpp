#include "xenon/cpu/function_compiler.hpp"

#include "xenon/cpu/optimizer.hpp"

namespace xenon::cpu {

FunctionCompileResult StaticFunctionCompiler::compile(
    GuestAddress base, std::span<const std::uint32_t> words) const {
  FunctionCompileResult out{};
  out.function.guest_address = base;
  out.function.blocks.reserve(words.size());

  for (std::size_t n = 0; n < words.size(); ++n) {
    const GuestAddress address = static_cast<GuestAddress>(base + n * 4u);
    const std::uint32_t word = words[n];
    auto decoded = decoder_.decode(address, word);
    if (!decoded.valid()) {
      out.error_address = address;
      out.error_word = word;
      out.error = "unknown Xenon/PPC instruction";
      return out;
    }

    ir::Block block{address, {}};
    ir::Builder builder(block);
    if (!lifter_.lift(decoded, builder) || block.instructions.empty()) {
      out.error_address = address;
      out.error_word = word;
      out.error = "recognized instruction has no Xenon IR lowering";
      return out;
    }
    out.function.blocks.push_back(std::move(block));
  }

  ir::Optimizer optimizer;
  (void)optimizer.run(out.function);
  out.ok = true;
  return out;
}

}  // namespace xenon::cpu
