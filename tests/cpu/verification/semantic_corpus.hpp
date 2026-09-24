#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xenon/cpu/types.hpp"

namespace xenon::cpu::verify::test_corpus {

struct InstructionSpec {
  const char* mnemonic;
  GuestAddress address;
  std::uint32_t operand_bits;
};

struct CorpusCase {
  const char* name;
  std::vector<InstructionSpec> instructions;
  bool memory_setup{};
};

inline std::vector<CorpusCase> cases() {
  return {
      {"sem_addi", {{"addi", 0x9000u, (5u << 21) | (3u << 16) | 0x7F01u}}},
      {"sem_addis", {{"addis", 0x9010u, (5u << 21) | (3u << 16) | 0x8123u}}},
      {"sem_ori", {{"ori", 0x9020u, (3u << 21) | (5u << 16) | 0xA55Au}}},
      {"sem_xoris", {{"xoris", 0x9030u, (3u << 21) | (5u << 16) | 0x1357u}}},
      {"sem_andi", {{"andix", 0x9040u, (3u << 21) | (5u << 16) | 0x5AA5u}}},
      {"sem_add_record", {{"addx", 0x9050u, (5u << 21) | (3u << 16) | (4u << 11) | 1u}}},
      {"sem_subf_record", {{"subfx", 0x9060u, (5u << 21) | (3u << 16) | (4u << 11) | 1u}}},
      {"sem_extsw", {{"extswx", 0x9070u, (3u << 21) | (5u << 16) | 1u}}},
      {"sem_cmpi", {{"cmpi", 0x9080u, (2u << 23) | (3u << 16) | 0xFF80u}}},
      {"sem_cmpld", {{"cmpl", 0x9090u, (2u << 23) | (1u << 21) | (3u << 16) | (4u << 11)}}},
      {"sem_cntlzw", {{"cntlzwx", 0x90A0u, (3u << 21) | (5u << 16) | 1u}}},
      {"sem_lwz", {{"lwz", 0x90B0u, (5u << 21) | (3u << 16) | 0x20u}}, true},
      {"sem_stw", {{"stw", 0x90C0u, (5u << 21) | (3u << 16) | 0x24u}}, true},
      {"sem_ld", {{"ld", 0x90D0u, (5u << 21) | (3u << 16) | 0x28u}}, true},
      {"sem_std", {{"std", 0x90E0u, (5u << 21) | (3u << 16) | 0x30u}}, true},
      {"sem_fmr", {{"fmrx", 0x90F0u, (5u << 21) | (3u << 11)}}},
      {"sem_fneg", {{"fnegx", 0x9100u, (5u << 21) | (3u << 11)}}},
      {"sem_vxor", {{"vxor", 0x9110u, (5u << 21) | (3u << 16) | (4u << 11)}}},
      {"sem_vor", {{"vor", 0x9120u, (5u << 21) | (3u << 16) | (4u << 11)}}},
      {"sem_branch", {{"bx", 0x9130u, 0x20u}}},
      {"sem_blr", {{"bclrx", 0x9140u, (20u << 21)}}},
      {"sem_bcctr", {{"bcctrx", 0x9150u, (20u << 21)}}},

      // Short-block fuzz targets exercise state forwarding and memory side
      // effects across multiple translated instructions, not just isolated
      // opcode handlers.
      {"sem_block_scalar",
       {{"addi", 0x9200u, (5u << 21) | (3u << 16) | 7u},
        {"ori", 0x9204u, (5u << 21) | (6u << 16) | 0x55u},
        {"addx", 0x9208u, (7u << 21) | (6u << 16) | (4u << 11) | 1u}}},
      {"sem_block_memory",
       {{"lwz", 0x9210u, (5u << 21) | (3u << 16) | 0x40u},
        {"xori", 0x9214u, (5u << 21) | (5u << 16) | 0x55AAu},
        {"stw", 0x9218u, (5u << 21) | (3u << 16) | 0x44u}}, true},
      {"sem_block_fp_vector",
       {{"fmrx", 0x9220u, (5u << 21) | (3u << 11)},
        {"fnegx", 0x9224u, (6u << 21) | (5u << 11)},
        {"vxor", 0x9228u, (5u << 21) | (3u << 16) | (4u << 11)}}},
  };
}

}  // namespace xenon::cpu::verify::test_corpus
