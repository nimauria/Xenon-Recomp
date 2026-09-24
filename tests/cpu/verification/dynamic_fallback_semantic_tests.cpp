#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <span>
#include <vector>

#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/dynamic_fallback.hpp"
#include "xenon/cpu/flat_memory.hpp"
#include "xenon/cpu/semantic_verifier.hpp"
#include "xenon/memory/address_space.hpp"
#include "semantic_corpus.hpp"

using namespace xenon::cpu;
using namespace xenon::cpu::verify;
using namespace xenon::cpu::verify::test_corpus;
using namespace xenon::memory;

namespace {
constexpr GuestAddress kCode = 0x00640000u;
constexpr GuestAddress kData = 0x00650000u;
constexpr GuestAddress kReturn = 0x0064F000u;
constexpr std::size_t kDataSize = 0x1000u;

const OpcodeInfo* find_opcode(std::string_view mnemonic) {
  for (const auto& op : Decoder::opcode_catalog())
    if (op.mnemonic == mnemonic) return &op;
  return nullptr;
}

void randomize_state(CpuState& state, DeterministicRng& rng) {
  for (auto& value : state.gpr) value = rng.next_u64();
  for (auto& value : state.fpr_bits) value = rng.next_u64();
  for (auto& vector : state.vr)
    for (auto& byte : vector.bytes) byte = rng.next_u8();
  state.lr = kReturn;
  state.ctr = rng.next_u64();
  state.cr = rng.next_u32();
  state.xer = rng.next_u32();
  state.fpscr = rng.next_u32();
  state.vscr = rng.next_u32();
  state.msr = rng.next_u64();
  state.vrsave = rng.next_u32();
  state.pvr = rng.next_u32();
  state.time_base = rng.next_u64();
}

bool same_state(const CpuState& a, const CpuState& b) {
  if (a.gpr != b.gpr || a.fpr_bits != b.fpr_bits) return false;
  for (std::size_t i = 0; i < a.vr.size(); ++i)
    if (a.vr[i].bytes != b.vr[i].bytes) return false;
  return a.lr == b.lr && a.ctr == b.ctr && a.cr == b.cr && a.xer == b.xer &&
         a.fpscr == b.fpscr && a.vscr == b.vscr && a.msr == b.msr &&
         a.vrsave == b.vrsave && a.pvr == b.pvr && a.time_base == b.time_base &&
         a.cia == b.cia && a.nia == b.nia;
}
}  // namespace

int main() {
  Decoder decoder;
  constexpr std::uint32_t kBlr = 0x4E800020u;

  // This is deliberately the overlap between the independent Gen 8 oracle and
  // Gen 7's scalar safety net. FPR/VMX remain AOT-vs-reference until fallback
  // itself intentionally grows those instruction families.
  const std::vector<std::string_view> names = {
      "sem_addi", "sem_addis", "sem_ori", "sem_xoris", "sem_andi",
      "sem_add_record", "sem_subf_record", "sem_extsw", "sem_cmpi",
      "sem_cmpld", "sem_cntlzw", "sem_lwz", "sem_stw", "sem_ld", "sem_std"};

  std::size_t trials = 0;
  const auto corpus = cases();
  AddressSpace fallback_memory;
  assert(fallback_memory.initialize());
  assert(fallback_memory.commit_fixed(kCode, kBasePageSize, kReadWriteExecute));
  assert(fallback_memory.commit_fixed(kData, kBasePageSize, kReadWrite));
  for (const auto wanted : names) {
    const auto it = std::find_if(corpus.begin(), corpus.end(), [&](const auto& c) {
      return std::string_view(c.name) == wanted && c.instructions.size() == 1;
    });
    assert(it != corpus.end());
    const auto& spec = it->instructions.front();
    const auto* info = find_opcode(spec.mnemonic);
    assert(info);
    const auto word = info->pattern | spec.operand_bits;
    const auto ref_instruction = decoder.decode(kCode, word);
    const auto ref_blr = decoder.decode(kCode + 4u, kBlr);
    assert(ref_instruction.valid() && ref_blr.valid());
    assert(ReferenceExecutor::supports(ref_instruction));
    assert(ReferenceExecutor::supports(ref_blr));

    for (std::size_t trial = 0; trial < 64; ++trial) {
      DeterministicRng rng(0x47454E3846414C4Cull ^
                           (static_cast<std::uint64_t>(trials + 1u) *
                            0x9E3779B97F4A7C15ull));
      CpuState initial{};
      randomize_state(initial, rng);
      if (it->memory_setup) initial.gpr[3] = kData + 0x180u;

      std::vector<std::uint8_t> data(kDataSize);
      for (auto& byte : data) byte = rng.next_u8();

      FlatMemory reference_memory(kDataSize, kData);
      reference_memory.data() = data;
      CpuState reference_state = initial;
      NullRuntimeServices reference_runtime;
      const std::array<DecodedInstruction, 2> reference_block{ref_instruction, ref_blr};
      const auto reference_result = ReferenceExecutor::execute_block(
          reference_block, reference_state, reference_memory, reference_runtime);

      fallback_memory.write32_be(kCode, word);
      fallback_memory.write32_be(kCode + 4u, kBlr);
      fallback_memory.write_bytes(
          kData, std::as_bytes(std::span<const std::uint8_t>(data)));

      CpuState fallback_state = initial;
      NullRuntimeServices fallback_runtime;
      DynamicFallbackConfig fallback_config{};
      fallback_config.max_instructions_per_dispatch = 8u;
      DynamicFallbackExecutor fallback(fallback_config);
      ExecutionContext context(fallback_state, fallback_memory, fallback_runtime);
      fallback.bind(context);
      const auto actual = context.try_dynamic_fallback(kCode, CompiledLookupKind::Call);
      if (!actual.handled || actual.result.reason != reference_result.reason ||
          actual.result.next_address != reference_result.next_address ||
          !same_state(reference_state, fallback_state)) {
        std::cerr << "Gen 7/reference mismatch: " << wanted << " trial=" << trial << "\n";
        std::cerr << "handled=" << actual.handled
                  << " expected_reason=" << static_cast<int>(reference_result.reason)
                  << " actual_reason=" << static_cast<int>(actual.result.reason)
                  << " expected_next=0x" << std::hex << reference_result.next_address
                  << " actual_next=0x" << actual.result.next_address << std::dec << "\n";
        if (reference_state.gpr != fallback_state.gpr) {
          for (std::size_t ri=0;ri<32;++ri) if(reference_state.gpr[ri]!=fallback_state.gpr[ri])
            std::cerr << "gpr"<<ri<<" exp=0x"<<std::hex<<reference_state.gpr[ri]<<" act=0x"<<fallback_state.gpr[ri]<<std::dec<<"\n";
        }
        std::cerr << "cia exp=0x" << std::hex << reference_state.cia << " act=0x" << fallback_state.cia
                  << " nia exp=0x" << reference_state.nia << " act=0x" << fallback_state.nia
                  << " lr exp=0x" << reference_state.lr << " act=0x" << fallback_state.lr
                  << " ctr exp=0x" << reference_state.ctr << " act=0x" << fallback_state.ctr
                  << " cr exp=0x" << reference_state.cr << " act=0x" << fallback_state.cr
                  << " xer exp=0x" << reference_state.xer << " act=0x" << fallback_state.xer << std::dec << "\n";
        return 2;
      }
      for (std::size_t index = 0; index < data.size(); ++index) {
        if (fallback_memory.read8(static_cast<GuestAddress>(kData + index)) !=
            reference_memory.data()[index]) {
          std::cerr << "Gen 7/reference memory mismatch: " << wanted
                    << " trial=" << trial << " offset=" << index << "\n";
          return 3;
        }
      }
      ++trials;
    }
  }

  std::cout << "xenon_dynamic_fallback_semantic_tests: " << trials
            << " reference/fallback trials: ok\n";
  return 0;
}
