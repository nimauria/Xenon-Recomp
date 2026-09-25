#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <variant>
#include <vector>

#include "xenon/gpu/command_processor.hpp"
#include "xenon/gpu/graphics_system.hpp"
#include "xenon/gpu/types.hpp"
#include "xenon/memory/address_space.hpp"

using xenon::gpu::CommandProcessor;
using xenon::gpu::DrawSource;
using xenon::gpu::Endian;
using xenon::gpu::IndexFormat;
using xenon::gpu::MajorMode;
using xenon::gpu::PacketType;
using xenon::gpu::PrimitiveType;
using xenon::gpu::RegisterFile;
using xenon::gpu::Type3Opcode;
using xenon::gpu::decode_packet_header;
using xenon::gpu::make_packet_type0;
using xenon::gpu::make_packet_type1;
using xenon::gpu::make_packet_type2;
using xenon::gpu::make_packet_type3;
using xenon::memory::AddressSpace;

namespace {

void store_be32(AddressSpace& memory, std::uint32_t physical_address,
                std::uint32_t value) {
  const std::array<std::byte, 4> bytes = {
      static_cast<std::byte>(value >> 24),
      static_cast<std::byte>(value >> 16),
      static_cast<std::byte>(value >> 8),
      static_cast<std::byte>(value),
  };
  assert(memory.write_physical(physical_address, bytes));
}

void store_le32(AddressSpace& memory, std::uint32_t physical_address,
                std::uint32_t value) {
  const std::array<std::byte, 4> bytes = {
      static_cast<std::byte>(value),
      static_cast<std::byte>(value >> 8),
      static_cast<std::byte>(value >> 16),
      static_cast<std::byte>(value >> 24),
  };
  assert(memory.write_physical(physical_address, bytes));
}
std::uint32_t load_le32_physical(AddressSpace& memory, std::uint32_t physical_address) {
  std::array<std::byte, 4> bytes{};
  assert(memory.copy_physical_range(physical_address, bytes));
  return std::to_integer<std::uint32_t>(bytes[0]) |
         (std::to_integer<std::uint32_t>(bytes[1]) << 8u) |
         (std::to_integer<std::uint32_t>(bytes[2]) << 16u) |
         (std::to_integer<std::uint32_t>(bytes[3]) << 24u);
}


std::uint32_t load_physical_be32(AddressSpace& memory,
                                 std::uint32_t physical_address) {
  std::array<std::byte, 4> bytes{};
  assert(memory.copy_physical_range(physical_address, bytes));
  return (std::uint32_t(std::to_integer<std::uint8_t>(bytes[0])) << 24u) |
         (std::uint32_t(std::to_integer<std::uint8_t>(bytes[1])) << 16u) |
         (std::uint32_t(std::to_integer<std::uint8_t>(bytes[2])) << 8u) |
         std::uint32_t(std::to_integer<std::uint8_t>(bytes[3]));
}

std::uint16_t load_physical_be16(AddressSpace& memory,
                                 std::uint32_t physical_address) {
  std::array<std::byte, 2> bytes{};
  assert(memory.copy_physical_range(physical_address, bytes));
  return static_cast<std::uint16_t>(
      (std::uint16_t(std::to_integer<std::uint8_t>(bytes[0])) << 8u) |
      std::uint16_t(std::to_integer<std::uint8_t>(bytes[1])));
}

void write_words(AddressSpace& memory, std::uint32_t base,
                 std::initializer_list<std::uint32_t> words) {
  std::uint32_t address = base;
  for (auto word : words) {
    store_be32(memory, address, word);
    address += 4;
  }
}

constexpr std::uint32_t make_draw_initiator(
    PrimitiveType primitive, DrawSource source, std::uint32_t index_count,
    IndexFormat format = IndexFormat::UInt16,
    MajorMode major_mode = MajorMode::Implicit, bool not_eop = false) {
  return static_cast<std::uint32_t>(primitive) |
         (static_cast<std::uint32_t>(source) << 6) |
         (static_cast<std::uint32_t>(major_mode) << 8) |
         (static_cast<std::uint32_t>(format) << 11) |
         (static_cast<std::uint32_t>(not_eop) << 12) |
         (index_count << 16);
}

void test_headers() {
  {
    const auto h = decode_packet_header(make_packet_type0(0x1234, 3));
    assert(h.type == PacketType::Type0);
    assert(h.count == 3);
    assert(h.register_index == 0x1234);
    assert(!h.write_one_register);
  }
  {
    const auto h = decode_packet_header(make_packet_type0(0x1234, 2, true));
    assert(h.write_one_register);
  }
  {
    const auto h = decode_packet_header(make_packet_type1(0x123, 0x456));
    assert(h.type == PacketType::Type1);
    assert(h.count == 2);
    assert(h.register_index_1 == 0x123);
    assert(h.register_index_2 == 0x456);
  }
  assert(decode_packet_header(make_packet_type2()).type == PacketType::Type2);
  {
    const auto h = decode_packet_header(make_packet_type3(Type3Opcode::MemWrite, 4, true));
    assert(h.type == PacketType::Type3);
    assert(h.count == 4);
    assert(h.opcode == Type3Opcode::MemWrite);
    assert(h.predicate);
  }

  assert(!xenon::gpu::is_explicit_major_mode(
      MajorMode::Implicit, PrimitiveType::TriangleList));
  assert(xenon::gpu::is_explicit_major_mode(
      MajorMode::Explicit, PrimitiveType::TriangleList));
  assert(xenon::gpu::is_explicit_major_mode(
      MajorMode::Reserved2, PrimitiveType::TriangleList));
  assert(xenon::gpu::is_explicit_major_mode(
      MajorMode::Implicit, PrimitiveType::CopyRectList0));
}

void test_register_packets(AddressSpace& memory, CommandProcessor& cp,
                           RegisterFile& regs) {
  constexpr std::uint32_t base = 0x01000000;
  write_words(memory, base,
              {make_packet_type0(0x100, 3), 0x11111111, 0x22222222, 0x33333333,
               make_packet_type0(0x110, 2, true), 0xAAAA0001, 0xAAAA0002,
               make_packet_type1(0x120, 0x121), 0xABCDEF01, 0x10203040,
               make_packet_type2()});
  cp.execute_buffer(base, 11);
  assert(regs.read(0x100) == 0x11111111);
  assert(regs.read(0x101) == 0x22222222);
  assert(regs.read(0x102) == 0x33333333);
  assert(regs.read(0x110) == 0xAAAA0002);
  assert(regs.read(0x120) == 0xABCDEF01);
  assert(regs.read(0x121) == 0x10203040);
}

void test_constants(AddressSpace& memory, CommandProcessor& cp,
                    RegisterFile& regs) {
  constexpr std::uint32_t base = 0x01001000;
  write_words(memory, base,
              {make_packet_type3(Type3Opcode::SetConstant, 3),
               0x00000005, 0x11112222, 0x33334444,
               make_packet_type3(Type3Opcode::SetConstant2, 3),
               0x00002010, 0xA0A0A0A0, 0xB0B0B0B0,
               make_packet_type3(Type3Opcode::SetShaderConstants, 2),
               0x00004900, 0x01020304});
  cp.execute_buffer(base, 11);
  assert(regs.read(0x4005) == 0x11112222);
  assert(regs.read(0x4006) == 0x33334444);
  assert(regs.read(0x2010) == 0xA0A0A0A0);
  assert(regs.read(0x2011) == 0xB0B0B0B0);
  assert(regs.read(0x4900) == 0x01020304);
}

void test_type3_predication(AddressSpace& memory, CommandProcessor& cp,
                            RegisterFile& regs,
                            xenon::gpu::ir::Stream& stream) {
  constexpr std::uint32_t failed = 0x01001400;
  constexpr std::uint32_t passed = 0x01001500;
  constexpr std::uint32_t test_register = 0x2F00;
  constexpr std::uint32_t initiator = make_draw_initiator(
      PrimitiveType::TriangleList, DrawSource::AutoIndex, 3);

  write_words(memory, failed,
              {make_packet_type3(Type3Opcode::SetBinMask, 2), 1u, 0u,
               make_packet_type3(Type3Opcode::SetBinSelect, 2), 0u, 0u,
               make_packet_type3(Type3Opcode::SetConstant2, 2, true),
               test_register, 0xDEADBEEFu,
               make_packet_type3(Type3Opcode::DrawIndx2, 1, true), initiator});
  const auto skipped_before = cp.statistics().predicated_packets_skipped;
  const auto draws_before = cp.statistics().draws;
  const auto stream_before = stream.size();
  cp.execute_buffer(failed, 11);
  assert(regs.read(test_register) == 0);
  assert(cp.statistics().draws == draws_before);
  assert(cp.statistics().predicated_packets_skipped == skipped_before + 2);
  // Only the two unpredicated bin-state packets are observable in IR.
  assert(stream.size() == stream_before + 2);

  write_words(memory, passed,
              {make_packet_type3(Type3Opcode::SetBinSelect, 2), 1u, 0u,
               make_packet_type3(Type3Opcode::SetConstant2, 2, true),
               test_register, 0xCAFEBABEu,
               make_packet_type3(Type3Opcode::DrawIndx2, 1, true), initiator});
  cp.execute_buffer(passed, 8);
  assert(regs.read(test_register) == 0xCAFEBABEu);
  assert(cp.statistics().draws == draws_before + 1);
  assert(cp.statistics().predicated_packets_skipped == skipped_before + 2);
  const auto& draw =
      std::get<xenon::gpu::ir::DrawPacket>(stream.commands().back());
  assert(draw.predicate);
}

void test_load_constant_context(AddressSpace& memory, CommandProcessor& cp,
                                RegisterFile& regs) {
  constexpr std::uint32_t constants = 0x01100000;
  constexpr std::uint32_t commands = 0x01002000;
  write_words(memory, constants, {0xDEADBEEF, 0x13579BDF, 0xCAFEBABE});
  write_words(memory, commands,
              {make_packet_type3(Type3Opcode::LoadConstantContext, 3),
               constants, (4u << 16) | 0x20u, 3});
  cp.execute_buffer(commands, 4);
  assert(regs.read(0x2020) == 0xDEADBEEF);
  assert(regs.read(0x2021) == 0x13579BDF);
  assert(regs.read(0x2022) == 0xCAFEBABE);
}

void test_pm4_reg_rmw(AddressSpace& memory, CommandProcessor& cp,
                      RegisterFile& regs) {
  constexpr std::uint32_t commands = 0x01002400;
  constexpr std::uint32_t target_immediate = 0x300u;
  constexpr std::uint32_t target_register = 0x301u;
  constexpr std::uint32_t and_register = 0x302u;
  constexpr std::uint32_t or_register = 0x303u;
  write_words(
      memory, commands,
      {make_packet_type0(target_immediate, 1), 0xF0F0FF00u,
       make_packet_type0(target_register, 1), 0xFFFF0000u,
       make_packet_type0(and_register, 1), 0x0F0FF0F0u,
       make_packet_type0(or_register, 1), 0x000000AAu,
       make_packet_type3(Type3Opcode::RegRmw, 3), target_immediate,
       0xFF00FFFFu, 0x00120034u,
       make_packet_type3(Type3Opcode::RegRmw, 3),
       0xC0000000u | target_register, and_register, or_register});
  const auto before = cp.statistics().register_rmw_packets;
  cp.execute_buffer(commands, 16);
  assert(regs.read(target_immediate) == 0xF012FF34u);
  assert(regs.read(target_register) == 0x0F0F00AAu);
  assert(cp.statistics().register_rmw_packets == before + 2u);
}

void test_pm4_reg_to_mem(AddressSpace& memory, CommandProcessor& cp,
                         RegisterFile& regs) {
  constexpr std::uint32_t commands = 0x01002600;
  constexpr std::uint32_t target = 0x01210000;
  constexpr std::uint32_t source_register = 0x310u;
  write_words(memory, commands,
              {make_packet_type0(source_register, 1), 0x11223344u,
               make_packet_type3(Type3Opcode::RegToMem, 2), source_register,
               target | static_cast<std::uint32_t>(Endian::Swap8In32)});
  const auto before_packets = cp.statistics().register_to_memory_packets;
  const auto before_writes = cp.statistics().physical_writes;
  cp.execute_buffer(commands, 5);
  assert(regs.read(source_register) == 0x11223344u);
  assert(memory.read32_be(xenon::memory::kPhysical64KBase + target) ==
         0x11223344u);
  assert(cp.statistics().register_to_memory_packets == before_packets + 1u);
  assert(cp.statistics().physical_writes == before_writes + 1u);
}

void test_pm4_reg_to_mem_loop(AddressSpace& memory, CommandProcessor& cp,
                              RegisterFile& regs) {
  constexpr std::uint32_t commands = 0x01002700;
  constexpr std::uint32_t target = 0x01210800;
  constexpr std::uint32_t first_register = 0x314u;
  write_words(memory, commands,
              {make_packet_type0(first_register, 3),
               0x11112222u, 0x33334444u, 0x55556666u,
               make_packet_type3(Type3Opcode::RegToMem, 2),
               first_register | (3u << 18u), target});
  const auto before_dwords = cp.statistics().register_to_memory_dwords;
  cp.execute_buffer(commands, 7);
  assert(regs.read(first_register + 0u) == 0x11112222u);
  assert(regs.read(first_register + 1u) == 0x33334444u);
  assert(regs.read(first_register + 2u) == 0x55556666u);
  assert(memory.read32_le(xenon::memory::kPhysical64KBase + target + 0u) ==
         0x11112222u);
  assert(memory.read32_le(xenon::memory::kPhysical64KBase + target + 4u) ==
         0x33334444u);
  assert(memory.read32_le(xenon::memory::kPhysical64KBase + target + 8u) ==
         0x55556666u);
  assert(cp.statistics().register_to_memory_dwords == before_dwords + 3u);
}

void test_pm4_cond_exec(AddressSpace& memory, CommandProcessor& cp,
                        RegisterFile& regs) {
  constexpr std::uint32_t condition0 = 0x01218000;
  constexpr std::uint32_t condition1 = 0x01218004;
  constexpr std::uint32_t false_commands = 0x01002D00;
  constexpr std::uint32_t true_commands = 0x01002E00;
  constexpr std::uint32_t skipped_register = 0x338u;
  constexpr std::uint32_t continued_register = 0x339u;
  constexpr std::uint32_t taken_register = 0x33Au;

  store_le32(memory, condition0, 0u);
  store_le32(memory, condition1, 5u);
  write_words(memory, false_commands,
              {make_packet_type3(Type3Opcode::CondExec, 4),
               condition0 >> 2u, condition1 >> 2u, 10u, 2u,
               make_packet_type0(skipped_register, 1), 0xAAAAAAAAu,
               make_packet_type0(continued_register, 1), 0xBBBBBBBBu});
  const auto before_packets = cp.statistics().conditional_exec_packets;
  const auto before_skipped = cp.statistics().conditional_exec_dwords_skipped;
  cp.execute_buffer(false_commands, 9);
  assert(regs.read(skipped_register) == 0u);
  assert(regs.read(continued_register) == 0xBBBBBBBBu);
  assert(cp.statistics().conditional_exec_packets == before_packets + 1u);
  assert(cp.statistics().conditional_exec_dwords_skipped == before_skipped + 2u);

  store_le32(memory, condition0, 1u);
  store_le32(memory, condition1, 5u);
  write_words(memory, true_commands,
              {make_packet_type3(Type3Opcode::CondExec, 4),
               condition0 >> 2u, condition1 >> 2u, 10u, 2u,
               make_packet_type0(taken_register, 1), 0xCCCCCCCCu});
  const auto before_taken = cp.statistics().conditional_exec_taken;
  cp.execute_buffer(true_commands, 7);
  assert(regs.read(taken_register) == 0xCCCCCCCCu);
  assert(cp.statistics().conditional_exec_taken == before_taken + 1u);
}

void test_pm4_cond_write(AddressSpace& memory, CommandProcessor& cp,
                         RegisterFile& regs) {
  constexpr std::uint32_t commands = 0x01002800;
  constexpr std::uint32_t poll_memory = 0x01211000;
  constexpr std::uint32_t write_memory = 0x01212000;
  constexpr std::uint32_t poll_register = 0x320u;
  constexpr std::uint32_t pass_register = 0x321u;
  constexpr std::uint32_t fail_register = 0x322u;
  store_be32(memory, poll_memory, 0xABCD1234u);
  write_words(
      memory, commands,
      {make_packet_type0(poll_register, 1), 5u,
       make_packet_type3(Type3Opcode::CondWrite, 6), 0x3u, poll_register, 5u,
       0xFFFFFFFFu, pass_register, 0xCAFEBABEu,
       make_packet_type3(Type3Opcode::CondWrite, 6), 0x3u, poll_register, 6u,
       0xFFFFFFFFu, fail_register, 0xDEADBEEFu,
       make_packet_type3(Type3Opcode::CondWrite, 6), 0x113u,
       poll_memory | static_cast<std::uint32_t>(Endian::Swap8In32),
       0xABCD1234u, 0xFFFFFFFFu,
       write_memory | static_cast<std::uint32_t>(Endian::Swap8In32),
       0x55667788u});
  const auto before_packets = cp.statistics().conditional_write_packets;
  const auto before_taken = cp.statistics().conditional_writes_taken;
  cp.execute_buffer(commands, 23);
  assert(regs.read(pass_register) == 0xCAFEBABEu);
  assert(regs.read(fail_register) == 0u);
  assert(memory.read32_be(xenon::memory::kPhysical64KBase + write_memory) ==
         0x55667788u);
  assert(cp.statistics().conditional_write_packets == before_packets + 3u);
  assert(cp.statistics().conditional_writes_taken == before_taken + 2u);
}

void test_pm4_wait_reg_mem(AddressSpace& memory, CommandProcessor& cp) {
  constexpr std::uint32_t commands = 0x01002A00;
  constexpr std::uint32_t poll_memory = 0x01213000;
  store_be32(memory, poll_memory, 0u);
  write_words(memory, commands,
              {make_packet_type3(Type3Opcode::WaitRegMem, 5), 0x13u,
               poll_memory | static_cast<std::uint32_t>(Endian::Swap8In32),
               0x12345678u, 0xFFFFFFFFu, 0u});

  std::atomic<bool> release_writer{false};
  std::thread writer([&] {
    while (!release_writer.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    store_be32(memory, poll_memory, 0x12345678u);
  });
  const auto before_packets = cp.statistics().wait_reg_mem_packets;
  const auto before_polls = cp.statistics().wait_reg_mem_polls;
  release_writer.store(true, std::memory_order_release);
  cp.execute_buffer(commands, 6);
  writer.join();
  assert(cp.statistics().wait_reg_mem_packets == before_packets + 1u);
  assert(cp.statistics().wait_reg_mem_polls > before_polls);
}

void test_pm4_compact_register_waits(AddressSpace& memory,
                                     CommandProcessor& cp,
                                     RegisterFile& regs) {
  constexpr std::uint32_t commands = 0x01002B00;
  constexpr std::uint32_t poll_register = 0x330u;
  write_words(
      memory, commands,
      {make_packet_type0(poll_register, 1), 0x00001234u,
       make_packet_type3(Type3Opcode::WaitRegEq, 4), poll_register, 0x1234u,
       0x0000FFFFu, 0u,
       make_packet_type3(Type3Opcode::WaitRegGte, 4), poll_register, 0x1200u,
       0x0000FFFFu, 0u});
  const auto before_packets = cp.statistics().wait_register_packets;
  const auto before_polls = cp.statistics().wait_register_polls;
  cp.execute_buffer(commands, 12);
  assert(regs.read(poll_register) == 0x00001234u);
  assert(cp.statistics().wait_register_packets == before_packets + 2u);
  assert(cp.statistics().wait_register_polls == before_polls + 2u);
}

void test_load_alu_constant(AddressSpace& memory, CommandProcessor& cp,
                            RegisterFile& regs) {
  constexpr std::uint32_t constants = 0x01214000;
  constexpr std::uint32_t commands = 0x01002C00;
  write_words(memory, constants, {0x10203040u, 0x50607080u});
  write_words(memory, commands,
              {make_packet_type3(Type3Opcode::LoadAluConstant, 3), constants,
               (1u << 16) | 0x10u, 2u});
  const auto before = cp.statistics().memory_constant_load_packets;
  cp.execute_buffer(commands, 4);
  assert(regs.read(0x4810u) == 0x10203040u);
  assert(regs.read(0x4811u) == 0x50607080u);
  assert(cp.statistics().memory_constant_load_packets == before + 1u);
}

void test_event_write_and_fences(AddressSpace& memory, CommandProcessor& cp,
                                 RegisterFile& regs) {
  constexpr std::uint32_t commands = 0x01002D00;
  constexpr std::uint32_t literal_target = 0x01215000;
  constexpr std::uint32_t counter_target = 0x01215010;
  write_words(
      memory, commands,
      {make_packet_type3(Type3Opcode::EventWrite, 1), 0x0000002Au,
       make_packet_type3(Type3Opcode::EventWriteShaderDone, 3), 0x00000006u,
       literal_target | static_cast<std::uint32_t>(Endian::Swap8In32),
       0x11223344u,
       make_packet_type3(Type3Opcode::EventWriteShaderDone, 3), 0x80000005u,
       counter_target | static_cast<std::uint32_t>(Endian::Swap8In32),
       0xDEADBEEFu});

  cp.notify_present();
  cp.notify_present();
  const auto before_events = cp.statistics().event_packets;
  const auto before_writes = cp.statistics().event_memory_writes;
  const auto before_counter = cp.statistics().event_counter_writes;
  cp.execute_buffer(commands, 10);

  assert(regs.read(0x21F9u) == 5u);
  assert(memory.read32_be(xenon::memory::kPhysical64KBase + literal_target) ==
         0x11223344u);
  assert(memory.read32_be(xenon::memory::kPhysical64KBase + counter_target) ==
         2u);
  assert(cp.swap_counter() == 2u);
  assert(cp.statistics().event_packets == before_events + 3u);
  assert(cp.statistics().event_memory_writes == before_writes + 2u);
  assert(cp.statistics().event_counter_writes == before_counter + 1u);
}

void test_event_extent(AddressSpace& memory, CommandProcessor& cp,
                       RegisterFile& regs) {
  constexpr std::uint32_t commands = 0x01002E00;
  constexpr std::uint32_t target = 0x01215100;
  write_words(memory, commands,
              {make_packet_type3(Type3Opcode::EventWriteExtent, 2), 0x09u,
               target | static_cast<std::uint32_t>(Endian::Swap8In16)});
  const auto before = cp.statistics().extent_event_writes;
  cp.execute_buffer(commands, 3);
  assert(regs.read(0x21F9u) == 9u);
  assert(load_physical_be16(memory, target + 0u) == 0u);
  assert(load_physical_be16(memory, target + 2u) == 1024u);
  assert(load_physical_be16(memory, target + 4u) == 0u);
  assert(load_physical_be16(memory, target + 6u) == 1024u);
  assert(load_physical_be16(memory, target + 8u) == 0u);
  assert(load_physical_be16(memory, target + 10u) == 1u);
  assert(cp.statistics().extent_event_writes == before + 1u);
}

void test_visibility_queries(AddressSpace& memory, CommandProcessor& cp,
                             RegisterFile& regs) {
  constexpr std::uint32_t commands = 0x01002F00;
  constexpr std::uint32_t initiator = make_draw_initiator(
      PrimitiveType::TriangleList, DrawSource::AutoIndex, 3);
  write_words(memory, commands,
              {make_packet_type3(Type3Opcode::VizQuery, 1), 3u,
               make_packet_type3(Type3Opcode::DrawIndx2, 1), initiator,
               make_packet_type3(Type3Opcode::VizQuery, 1), 0x103u,
               make_packet_type3(Type3Opcode::VizQuery, 1), 4u,
               make_packet_type3(Type3Opcode::VizQuery, 1), 0x104u});
  const auto begin_before = cp.statistics().viz_query_begins;
  const auto end_before = cp.statistics().viz_query_ends;
  const auto visible_before = cp.statistics().viz_query_visible_results;
  cp.execute_buffer(commands, 10);
  assert((regs.read(0x0C44u) & (1u << 3u)) != 0u);
  assert((regs.read(0x0C44u) & (1u << 4u)) == 0u);
  assert(regs.read(0x21F9u) == 8u);
  assert(cp.statistics().viz_query_begins == begin_before + 2u);
  assert(cp.statistics().viz_query_ends == end_before + 2u);
  assert(cp.statistics().viz_query_visible_results == visible_before + 1u);
}

void test_interrupt_dispatch(AddressSpace& memory, CommandProcessor& cp) {
  constexpr std::uint32_t commands = 0x01002F80;
  std::vector<std::uint32_t> dispatched;
  cp.set_interrupt_callback(
      [&](std::uint32_t cpu) { dispatched.push_back(cpu); });
  write_words(memory, commands,
              {make_packet_type3(Type3Opcode::Interrupt, 1),
               (1u << 1u) | (1u << 4u)});
  const auto packets_before = cp.statistics().interrupt_packets;
  const auto dispatch_before = cp.statistics().interrupt_dispatches;
  cp.execute_buffer(commands, 2);
  assert((dispatched == std::vector<std::uint32_t>{1u, 4u}));
  assert(cp.statistics().interrupt_packets == packets_before + 1u);
  assert(cp.statistics().interrupt_dispatches == dispatch_before + 2u);
  cp.set_interrupt_callback({});
}

void test_mem_write(AddressSpace& memory, CommandProcessor& cp) {
  constexpr std::uint32_t commands = 0x01003000;
  constexpr std::uint32_t target = 0x01200000;
  write_words(memory, commands,
              {make_packet_type3(Type3Opcode::MemWrite, 3),
               target | static_cast<std::uint32_t>(Endian::Swap8In32),
               0x11223344, 0xA1B2C3D4});
  const auto before_write = memory.coherency().current_epoch();
  cp.execute_buffer(commands, 4);
  const auto after_write = memory.coherency().current_epoch();
  // A multi-dword MEM_WRITE is one physical range transaction, not one
  // reservation/coherency publication per dword.
  assert(after_write == before_write + 1u);
  // k8in32 produces CPU-big-endian bytes for normal CPU-visible dwords.
  assert(memory.read32_be(xenon::memory::kPhysical64KBase + target) == 0x11223344);
  assert(memory.read32_be(xenon::memory::kPhysical64KBase + target + 4) == 0xA1B2C3D4);
}

void test_indirect_buffer(AddressSpace& memory, CommandProcessor& cp,
                          RegisterFile& regs) {
  constexpr std::uint32_t primary = 0x01004000;
  constexpr std::uint32_t indirect = 0x01005000;
  write_words(memory, indirect,
              {make_packet_type0(0x321, 1), 0x55667788});
  write_words(memory, primary,
              {make_packet_type3(Type3Opcode::IndirectBuffer, 2), indirect, 2});
  cp.execute_buffer(primary, 3);
  assert(regs.read(0x321) == 0x55667788);
  assert(cp.statistics().indirect_buffers >= 1);
  assert(cp.max_indirect_depth() == 1);
}

void test_ring_wrap(AddressSpace& memory, CommandProcessor& cp,
                    RegisterFile& regs) {
  constexpr std::uint32_t ring = 0x01006000;
  constexpr std::uint32_t capacity = 8;
  // Unread range is slots [6,7,0,1].
  store_be32(memory, ring + 6 * 4, make_packet_type1(0x330, 0x331));
  store_be32(memory, ring + 7 * 4, 0x11110000);
  store_be32(memory, ring + 0 * 4, 0x22220000);
  store_be32(memory, ring + 1 * 4, make_packet_type2());
  const auto new_read = cp.execute_ring(ring, capacity, 6, 2);
  assert(new_read == 2);
  assert(regs.read(0x330) == 0x11110000);
  assert(regs.read(0x331) == 0x22220000);
}

void test_draw_ir(AddressSpace& memory, CommandProcessor& cp,
                  RegisterFile& regs, xenon::gpu::ir::Stream& stream) {
  constexpr std::uint32_t base = 0x01007000;
  constexpr std::uint32_t initiator = make_draw_initiator(
      PrimitiveType::TriangleList, DrawSource::AutoIndex, 3);
  write_words(memory, base,
              {make_packet_type3(Type3Opcode::DrawIndx2, 1), initiator});
  const auto before = stream.size();
  cp.execute_buffer(base, 2);
  assert(cp.statistics().draws >= 1);
  // DRAW_INDX_2 implicitly writes VGT_DRAW_INITIATOR before the normalized draw.
  assert(stream.size() == before + 2);
  const auto& command = stream.commands().back();
  assert(std::holds_alternative<xenon::gpu::ir::DrawPacket>(command));
  const auto& draw = std::get<xenon::gpu::ir::DrawPacket>(command);
  assert(draw.opcode == Type3Opcode::DrawIndx2);
  assert(draw.primitive_type == PrimitiveType::TriangleList);
  assert(draw.source == DrawSource::AutoIndex);
  assert(draw.index_count == 3);
  assert(!draw.index_buffer.valid);
  assert(draw.raw_payload.size() == 1);
  assert(draw.raw_payload[0] == initiator);
  assert(regs.read(0x21FC) == initiator);
}

void test_dma_draw_ir(AddressSpace& memory, CommandProcessor& cp,
                      RegisterFile& regs, xenon::gpu::ir::Stream& stream) {
  constexpr std::uint32_t base = 0x01007400;
  constexpr std::uint32_t index_base = 0x01400000;
  constexpr std::uint32_t initiator = make_draw_initiator(
      PrimitiveType::TriangleStrip, DrawSource::Dma, 12, IndexFormat::UInt32,
      MajorMode::Explicit, true);
  constexpr std::uint32_t dma_size =
      (static_cast<std::uint32_t>(Endian::Swap8In32) << 30) | 12u;
  constexpr std::uint32_t viz_query = 0xA5A50001u;
  write_words(memory, base,
              {make_packet_type3(Type3Opcode::DrawIndx, 4), viz_query,
               initiator, index_base, dma_size});
  const auto before = stream.size();
  cp.execute_buffer(base, 5);
  // Initiator + DMA base + DMA size register writes, then the draw.
  assert(stream.size() == before + 4);
  const auto& draw =
      std::get<xenon::gpu::ir::DrawPacket>(stream.commands().back());
  assert(draw.primitive_type == PrimitiveType::TriangleStrip);
  assert(draw.source == DrawSource::Dma);
  assert(draw.explicit_major_mode);
  assert(draw.not_eop);
  assert(draw.index_format == IndexFormat::UInt32);
  assert(draw.index_count == 12);
  assert(draw.viz_query_condition == viz_query);
  assert(draw.index_buffer.valid);
  assert(draw.index_buffer.physical_address == index_base);
  assert(draw.index_buffer.length_bytes == 48);
  assert(draw.index_buffer.index_count == 12);
  assert(draw.index_buffer.format == IndexFormat::UInt32);
  assert(draw.index_buffer.endian == Endian::Swap8In32);
  assert(regs.read(0x21FA) == index_base);
  assert(regs.read(0x21FB) == dma_size);
  assert(regs.read(0x21FC) == initiator);
}



void test_binned_draw_ir(AddressSpace& memory, CommandProcessor& cp,
                         RegisterFile& regs, xenon::gpu::ir::Stream& stream) {
  constexpr std::uint32_t base = 0x01007800;
  constexpr std::uint32_t index_base = 0x01410000;
  constexpr std::uint32_t initiator = make_draw_initiator(
      PrimitiveType::TriangleList, DrawSource::Dma, 6, IndexFormat::UInt16);
  constexpr std::uint32_t dma_size =
      (static_cast<std::uint32_t>(Endian::Swap16In32) << 30) | 6u;
  constexpr std::uint32_t bin_offset = 0x20u;
  constexpr std::uint32_t bin_base = 0x120u;
  constexpr std::uint32_t bin_size = 6u;
  constexpr std::uint64_t bin_mask = UINT64_C(0x89ABCDEF01234567);
  constexpr std::uint64_t bin_select = UINT64_C(0x76543210FEDCBA98);
  constexpr std::uint32_t viz_query = 0xABCD0001u;

  write_words(memory, base,
              {make_packet_type3(Type3Opcode::SetBinBaseOffset, 1), bin_offset,
               make_packet_type3(Type3Opcode::SetBinMask, 2),
               static_cast<std::uint32_t>(bin_mask),
               static_cast<std::uint32_t>(bin_mask >> 32),
               make_packet_type3(Type3Opcode::SetBinSelect, 2),
               static_cast<std::uint32_t>(bin_select),
               static_cast<std::uint32_t>(bin_select >> 32),
               make_packet_type3(Type3Opcode::DrawIndxBin, 6), viz_query,
               initiator, bin_base, bin_size, index_base, dma_size});

  const auto before = stream.size();
  cp.execute_buffer(base, 15);
  // Three bin-state packets + initiator/DMA implicit writes + normalized draw.
  assert(stream.size() == before + 7);
  const auto& draw =
      std::get<xenon::gpu::ir::DrawPacket>(stream.commands().back());
  assert(draw.binned);
  assert(draw.opcode == Type3Opcode::DrawIndxBin);
  assert(draw.viz_query_condition == viz_query);
  assert(draw.binning.valid);
  assert(draw.binning.base == bin_base);
  assert(draw.binning.size == bin_size);
  assert(draw.binning.base_offset == bin_offset);
  assert(draw.binning.effective_base == bin_base + bin_offset);
  assert(draw.binning.mask == bin_mask);
  assert(draw.binning.select == bin_select);
  assert(draw.index_buffer.valid);
  assert(draw.index_buffer.physical_address == index_base);
  assert(draw.index_buffer.length_bytes == 12u);
  assert(draw.index_buffer.endian == Endian::Swap16In32);
  assert(regs.read(0x21FA) == index_base);
  assert(regs.read(0x21FB) == dma_size);
  assert(regs.read(0x21FC) == initiator);

  constexpr std::uint32_t base2 = 0x01007C00;
  constexpr std::uint32_t immediate_initiator = make_draw_initiator(
      PrimitiveType::LineList, DrawSource::Immediate, 4, IndexFormat::UInt16);
  write_words(memory, base2,
              {make_packet_type3(Type3Opcode::DrawIndx2Bin, 5),
               immediate_initiator, 0x200u, 4u,
               0x00010000u, 0x00030002u});
  cp.execute_buffer(base2, 6);
  const auto& immediate_draw =
      std::get<xenon::gpu::ir::DrawPacket>(stream.commands().back());
  assert(immediate_draw.binned);
  assert(immediate_draw.binning.valid);
  assert(immediate_draw.binning.base == 0x200u);
  assert(immediate_draw.binning.size == 4u);
  assert(immediate_draw.binning.base_offset == bin_offset);
  assert(immediate_draw.immediate_index_dwords.size() == 2u);
  assert(immediate_draw.immediate_index_dwords[0] == 0x00010000u);
  assert(immediate_draw.immediate_index_dwords[1] == 0x00030002u);
}

void test_shader_loads(AddressSpace& memory, CommandProcessor& cp,
                       xenon::gpu::ir::Stream& stream) {
  constexpr std::uint32_t shader_addr = 0x01300000;
  constexpr std::uint32_t pointer_cmd = 0x01009000;
  constexpr std::uint32_t immediate_cmd = 0x0100A000;
  const std::uint32_t code[6] = {
      0x01020304, 0x11121314, 0x21222324,
      0x31323334, 0x41424344, 0x51525354,
  };
  for (std::uint32_t i = 0; i < 6; ++i) store_be32(memory, shader_addr + i * 4, code[i]);

  write_words(memory, pointer_cmd,
              {make_packet_type3(Type3Opcode::ImLoad, 2),
               shader_addr | 0u, 6u});
  const auto before_pointer = stream.size();
  cp.execute_buffer(pointer_cmd, 3);
  assert(stream.size() == before_pointer + 1);
  assert(std::holds_alternative<xenon::gpu::ir::ShaderLoad>(stream.commands().back()));
  const auto pointer_load = std::get<xenon::gpu::ir::ShaderLoad>(stream.commands().back());
  assert(!pointer_load.immediate);
  assert(pointer_load.physical_address == shader_addr);
  assert(pointer_load.program.stage() == xenon::gpu::ShaderStage::Vertex);
  assert(pointer_load.program.instruction_count() == 2);
  assert(pointer_load.program.instruction(1).words[2] == 0x51525354);

  write_words(memory, immediate_cmd,
              {make_packet_type3(Type3Opcode::ImLoadImmediate, 8),
               0u, 6u,
               code[0], code[1], code[2], code[3], code[4], code[5]});
  const auto before_immediate = stream.size();
  cp.execute_buffer(immediate_cmd, 9);
  assert(stream.size() == before_immediate + 1);
  assert(std::holds_alternative<xenon::gpu::ir::ShaderLoad>(stream.commands().back()));
  const auto immediate_load = std::get<xenon::gpu::ir::ShaderLoad>(stream.commands().back());
  assert(immediate_load.immediate);
  assert(immediate_load.program.hash() == pointer_load.program.hash());
  assert(immediate_load.program.dwords().size() == 6);
}


void test_shader_partition_state(AddressSpace& memory, CommandProcessor& cp) {
  constexpr std::uint32_t commands = 0x0100A800;
  constexpr std::uint32_t raw = (5u << 29u) | (0x345u << 16u) | 0x123u;
  write_words(memory, commands,
              {make_packet_type3(Type3Opcode::SetShaderBases, 1), raw});
  const auto before = cp.statistics().shader_base_packets;
  cp.execute_buffer(commands, 2);
  const auto& state = cp.shader_partition();
  assert(state.raw == raw);
  assert(state.instruction_store_size_code == 5u);
  assert(state.vertex_start == 0x345u);
  assert(state.pixel_start == 0x123u);
  assert(cp.statistics().shader_base_packets == before + 1u);
}

void test_im_store_round_trip(AddressSpace& memory, CommandProcessor& cp,
                              xenon::gpu::ir::Stream& stream) {
  constexpr std::uint32_t commands = 0x0100AA00;
  constexpr std::uint32_t restore_commands = 0x0100AB00;
  constexpr std::uint32_t shader_shadow = 0x01310000;
  constexpr std::uint32_t metadata = 0x01311000;
  constexpr std::uint32_t start_slot = 7u;
  const std::array<std::uint32_t, 6> code = {
      0x01020304u, 0x11121314u, 0x21222324u,
      0x31323334u, 0x41424344u, 0x51525354u,
  };
  write_words(memory, commands,
              {make_packet_type3(Type3Opcode::ImLoadImmediate, 8),
               0u, (start_slot << 16u) | 6u,
               code[0], code[1], code[2], code[3], code[4], code[5],
               make_packet_type3(Type3Opcode::ImStore, 2),
               shader_shadow | 0u, metadata});
  const auto before_packets = cp.statistics().shader_store_packets;
  const auto before_dwords = cp.statistics().shader_store_dwords;
  cp.execute_buffer(commands, 12);
  for (std::uint32_t i = 0; i < code.size(); ++i) {
    assert(load_physical_be32(memory, shader_shadow + i * 4u) == code[i]);
  }
  assert(load_physical_be32(memory, metadata) ==
         ((start_slot << 16u) | 6u));
  assert(cp.statistics().shader_store_packets == before_packets + 1u);
  assert(cp.statistics().shader_store_dwords == before_dwords + 6u);

  write_words(memory, restore_commands,
              {make_packet_type3(Type3Opcode::ImLoad, 2),
               shader_shadow | 0u, (start_slot << 16u) | 6u});
  const auto before_restore = stream.size();
  cp.execute_buffer(restore_commands, 3);
  assert(stream.size() == before_restore + 1u);
  const auto& restore =
      std::get<xenon::gpu::ir::ShaderLoad>(stream.commands().back());
  assert(restore.program.stage() == xenon::gpu::ShaderStage::Vertex);
  assert(restore.program.start_slot() == start_slot);
  assert(restore.program.dwords().size() == code.size());
  for (std::uint32_t i = 0; i < code.size(); ++i) {
    assert(restore.program.dwords()[i] == code[i]);
  }
}

void test_shader_invalidation(AddressSpace& memory, CommandProcessor& cp,
                              xenon::gpu::ir::Stream& stream) {
  constexpr std::uint32_t commands = 0x0100AC00;
  constexpr std::uint32_t initiator = make_draw_initiator(
      PrimitiveType::TriangleList, DrawSource::AutoIndex, 3);
  write_words(memory, commands,
              {make_packet_type3(Type3Opcode::ImLoadImmediate, 5),
               0u, 3u, 0x01020304u, 0x11121314u, 0x21222324u,
               make_packet_type3(Type3Opcode::ImLoadImmediate, 5),
               1u, 3u, 0x31323334u, 0x41424344u, 0x51525354u,
               make_packet_type3(Type3Opcode::InvalidateState, 1), 0x100u,
               make_packet_type3(Type3Opcode::DrawIndx2, 1), initiator});
  const auto before_invalidations = cp.statistics().shader_invalidations;
  cp.execute_buffer(commands, 16);
  const auto& draw =
      std::get<xenon::gpu::ir::DrawPacket>(stream.commands().back());
  assert(!draw.vertex_shader.valid);
  assert(draw.pixel_shader.valid);
  assert(cp.statistics().shader_invalidations == before_invalidations + 1u);
}

void test_draw_shader_bindings(AddressSpace& memory, CommandProcessor& cp,
                               xenon::gpu::ir::Stream& stream) {
  constexpr std::uint32_t base = 0x0100C000;
  constexpr std::uint32_t vs0 = 0x01020304u;
  constexpr std::uint32_t vs1 = 0x11121314u;
  constexpr std::uint32_t vs2 = 0x21222324u;
  constexpr std::uint32_t ps0 = 0x31323334u;
  constexpr std::uint32_t ps1 = 0x41424344u;
  constexpr std::uint32_t ps2 = 0x51525354u;
  constexpr std::uint32_t initiator = make_draw_initiator(
      PrimitiveType::TriangleList, DrawSource::AutoIndex, 3);

  write_words(memory, base,
              {make_packet_type3(Type3Opcode::ImLoadImmediate, 5),
               0u, 3u, vs0, vs1, vs2,
               make_packet_type3(Type3Opcode::ImLoadImmediate, 5),
               1u, 3u, ps0, ps1, ps2,
               make_packet_type3(Type3Opcode::DrawIndx2, 1), initiator});

  const auto before = stream.size();
  cp.execute_buffer(base, 14);
  assert(stream.size() == before + 4);
  const auto& vs_load =
      std::get<xenon::gpu::ir::ShaderLoad>(stream.commands()[before]);
  const auto& ps_load =
      std::get<xenon::gpu::ir::ShaderLoad>(stream.commands()[before + 1]);
  const auto& draw =
      std::get<xenon::gpu::ir::DrawPacket>(stream.commands().back());
  assert(vs_load.program.stage() == xenon::gpu::ShaderStage::Vertex);
  assert(ps_load.program.stage() == xenon::gpu::ShaderStage::Pixel);
  assert(draw.vertex_shader.valid);
  assert(draw.pixel_shader.valid);
  assert(draw.vertex_shader.hash == vs_load.program.hash());
  assert(draw.pixel_shader.hash == ps_load.program.hash());
  assert(draw.vertex_shader.start_slot == 0);
  assert(draw.pixel_shader.start_slot == 0);
}

void test_immediate_draw_ir(AddressSpace& memory, CommandProcessor& cp,
                            xenon::gpu::ir::Stream& stream) {
  constexpr std::uint32_t base = 0x0100D000;
  constexpr std::uint32_t initiator = make_draw_initiator(
      PrimitiveType::LineList, DrawSource::Immediate, 4, IndexFormat::UInt16);
  write_words(memory, base,
              {make_packet_type3(Type3Opcode::DrawIndx2, 3), initiator,
               0x00010000u, 0x00030002u});
  const auto before = stream.size();
  cp.execute_buffer(base, 4);
  assert(stream.size() == before + 2);
  const auto& draw =
      std::get<xenon::gpu::ir::DrawPacket>(stream.commands().back());
  assert(draw.source == DrawSource::Immediate);
  assert(draw.index_count == 4);
  assert(draw.immediate_index_dwords.size() == 2);
  assert(draw.immediate_index_dwords[0] == 0x00010000u);
  assert(draw.immediate_index_dwords[1] == 0x00030002u);
}

void test_shader_control_flow_unpack() {
  const std::uint32_t a0 = 0x123u | (5u << 12) | (1u << 15) | (0xABCu << 16);
  const std::uint16_t a1 = static_cast<std::uint16_t>(
      (1u << 11) | (static_cast<std::uint16_t>(xenon::gpu::ControlFlowOpcode::Exec) << 12));
  const std::uint32_t b0 = 0x456u | (3u << 12) | (0x135u << 16);
  const std::uint16_t b1 = static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(xenon::gpu::ControlFlowOpcode::ExecEnd) << 12);
  const std::uint32_t packed[3] = {
      a0,
      std::uint32_t(a1) | ((b0 & 0xFFFFu) << 16),
      (b0 >> 16) | (std::uint32_t(b1) << 16),
  };
  xenon::gpu::ShaderProgram program(xenon::gpu::ShaderStage::Vertex, packed);
  const auto pair = program.control_flow_pair(0);
  const auto& a = pair.instructions[0];
  const auto& b = pair.instructions[1];
  assert(a.opcode == xenon::gpu::ControlFlowOpcode::Exec);
  assert(a.exec_address() == 0x123);
  assert(a.exec_count() == 5);
  assert(a.exec_yield());
  assert(a.exec_sequence() == 0xABC);
  assert(a.absolute_addressing());
  assert(b.opcode == xenon::gpu::ControlFlowOpcode::ExecEnd);
  assert(b.exec_address() == 0x456);
  assert(b.exec_count() == 3);
  assert(b.exec_sequence() == 0x135);
  assert(!b.absolute_addressing());
}

void test_edram() {
  xenon::gpu::Edram edram;
  assert(edram.bytes().size() == 10u * 1024u * 1024u);
  assert(edram.tile_address(xenon::gpu::Edram::kTileCount) == 0);
  const std::byte data[4] = {std::byte{0x11}, std::byte{0x22},
                             std::byte{0x33}, std::byte{0x44}};
  edram.write(xenon::gpu::Edram::kSize - 2u, data);
  assert(edram.read8(xenon::gpu::Edram::kSize - 2u) == 0x11);
  assert(edram.read8(xenon::gpu::Edram::kSize - 1u) == 0x22);
  assert(edram.read8(0) == 0x33);
  assert(edram.read8(1) == 0x44);
  edram.clear(xenon::gpu::Edram::kSize - 1u, 3u, 0xAA);
  assert(edram.read8(xenon::gpu::Edram::kSize - 1u) == 0xAA);
  assert(edram.read8(0) == 0xAA);
  assert(edram.read8(1) == 0xAA);
}

void test_graphics_system_backend(AddressSpace& memory) {
  xenon::gpu::GraphicsSystem graphics(memory);
  constexpr std::uint32_t base = 0x0100B000;
  constexpr std::uint32_t initiator = make_draw_initiator(
      PrimitiveType::TriangleStrip, DrawSource::AutoIndex, 6);
  write_words(memory, base,
              {make_packet_type0(0x44, 1), 0x12345678,
               make_packet_type3(Type3Opcode::DrawIndx2, 1), initiator});
  graphics.submit_buffer(base, 4);
  xenon::gpu::NullBackend backend;
  const auto pending = graphics.stream().size();
  graphics.execute_ir(backend);
  assert(backend.command_count() == pending);
  assert(backend.command_count() == 3);
  const auto counters = backend.performance_counters();
  assert(counters.submissions == 1);
  assert(counters.commands == 3);
  assert(counters.draws == 1);
  assert(graphics.stream().size() == 0);
  assert(graphics.registers().read(0x44) == 0x12345678);
  // Part 7 of the AC6 Runtime Readiness pass: the Backend base class default
  // (NullBackend does not override it) must report a real, honest zero -
  // not because a title triggered nothing unsupported, but because a
  // backend with no telemetry of its own must never fabricate a nonzero-
  // looking-clean total either.
  const auto unsupported = backend.unsupported_counters();
  assert(unsupported.total() == 0u);
  // Part 9: same honest-default requirement for shader coverage.
  const auto coverage = backend.shader_coverage();
  assert(coverage.shaders_discovered == 0u);
  assert(coverage.translation_failures == 0u);
  assert(coverage.cache_hits == 0u && coverage.cache_misses == 0u);
}

void test_frontend_capture_replay(AddressSpace& memory) {
  xenon::gpu::GraphicsSystem graphics(memory);
  constexpr std::uint32_t base = 0x0100B800;
  constexpr std::uint32_t initiator = make_draw_initiator(
      PrimitiveType::TriangleList, DrawSource::AutoIndex, 3);
  write_words(memory, base,
              {make_packet_type0(0x50, 1), 0xCAFED00Du,
               make_packet_type3(Type3Opcode::DrawIndx2, 1), initiator});

  const auto stream_before = graphics.stream().size();
  const auto capture = graphics.capture_buffer(base, 4);
  assert(capture.source ==
         xenon::gpu::FrontendSubmissionCapture::Source::Buffer);
  assert(capture.physical_address == base);
  assert(capture.dword_count == 4u);
  assert(capture.initial_registers.values[0x50] == 0u);
  assert(capture.final_registers.values[0x50] == 0xCAFED00Du);
  assert(capture.commands.size() == 3u);
  assert(graphics.stream().size() == stream_before + capture.commands.size());

  xenon::gpu::NullBackend replay_backend;
  graphics.replay_capture(replay_backend, capture);
  assert(replay_backend.command_count() ==
         RegisterFile::kRegisterCount + capture.commands.size());
  // Replay is a backend/debug operation and must not consume the live pending
  // stream that will still be executed normally by the runtime.
  assert(graphics.stream().size() == stream_before + capture.commands.size());
}


void test_portable_capture_round_trip(AddressSpace& memory) {
  xenon::gpu::GraphicsSystem graphics(memory);
  xenon::gpu::NullBackend backend;
  constexpr std::uint32_t shader_cmd = 0x0100C000;
  constexpr std::uint32_t capture_cmd = 0x0100C100;
  constexpr std::uint32_t side_effect = 0x01420000;
  constexpr std::uint32_t vertex_data = 0x01421000;
  constexpr std::uint32_t fetch_register =
      xenon::gpu::ResourceStateTracker::kFetchConstantBase + 3u * 6u + 2u * 2u;
  const std::uint32_t shader_code[3] = {0x01020304u, 0x11121314u, 0x21222324u};

  write_words(memory, shader_cmd,
              {make_packet_type3(Type3Opcode::ImLoadImmediate, 5),
               0u, 3u, shader_code[0], shader_code[1], shader_code[2]});
  graphics.submit_buffer(shader_cmd, 6u);

  constexpr std::uint32_t initiator = make_draw_initiator(
      PrimitiveType::TriangleList, DrawSource::AutoIndex, 3);
  std::array<std::byte, 64u * 4u> vertex_bytes{};
  for (std::size_t i = 0; i < vertex_bytes.size(); ++i)
    vertex_bytes[i] = static_cast<std::byte>(i & 0xFFu);
  assert(memory.write_physical(vertex_data, vertex_bytes));
  write_words(memory, capture_cmd,
              {make_packet_type3(Type3Opcode::MemWrite, 2),
               side_effect, 0xA1B2C3D4u,
               make_packet_type0(fetch_register, 2),
               3u | vertex_data, 64u << 2u,
               make_packet_type3(Type3Opcode::DrawIndx2, 1), initiator});
  graphics.edram().write8(0x1234u, 0x5Au);

  xenon::gpu::PortableSubmissionCapture capture;
  std::string error;
  assert(graphics.capture_portable_buffer(backend, capture_cmd, 8u, capture, &error));
  assert(error.empty());
  assert(capture.complete);
  assert(capture.frontend.initial_vertex_program.has_value());
  assert(capture.frontend.initial_vertex_program->hash() != 0u);
  assert(capture.edram.size() == xenon::gpu::Edram::kSize);
  assert(capture.edram[0x1234u] == std::byte{0x5A});
  assert(load_le32_physical(memory, side_effect) == 0xA1B2C3D4u);

  bool found_side_effect = false;
  bool found_command_stream = false;
  bool found_vertex_data = false;
  for (const auto& range : capture.physical_ranges) {
    const auto begin = range.physical_address;
    const auto end = std::uint64_t(begin) + range.bytes.size();
    if (side_effect >= begin && std::uint64_t(side_effect) + 4u <= end)
      found_side_effect = true;
    if (capture_cmd >= begin && std::uint64_t(capture_cmd) + 32u <= end)
      found_command_stream = true;
    if (vertex_data >= begin &&
        std::uint64_t(vertex_data) + vertex_bytes.size() <= end)
      found_vertex_data = true;
  }
  assert(found_side_effect);
  assert(found_command_stream);
  assert(found_vertex_data);

  const auto path = std::filesystem::temp_directory_path() /
                    "xenon_gpu_portable_capture_test.xgcap";
  std::filesystem::remove(path);
  assert(xenon::gpu::save_portable_capture(capture, path, &error));
  assert(error.empty());
  const auto loaded = xenon::gpu::load_portable_capture(path, &error);
  assert(loaded.has_value());
  assert(error.empty());
  assert(loaded->frontend.initial_vertex_program.has_value());
  assert(loaded->frontend.initial_vertex_program->hash() ==
         capture.frontend.initial_vertex_program->hash());
  assert(loaded->physical_byte_count() == capture.physical_byte_count());
  assert(loaded->edram == capture.edram);

  store_le32(memory, side_effect, 0u);
  graphics.edram().write8(0x1234u, 0u);
  xenon::gpu::NullBackend replay_backend;
  assert(graphics.replay_portable_capture(replay_backend, *loaded, &error));
  assert(error.empty());
  assert(load_le32_physical(memory, side_effect) == 0xA1B2C3D4u);
  assert(graphics.edram().read8(0x1234u) == 0x5Au);
  assert(replay_backend.command_count() ==
         RegisterFile::kRegisterCount + loaded->frontend.commands.size() + 1u);
  std::filesystem::remove(path);
}

void test_frontend_ring_capture(AddressSpace& memory) {
  xenon::gpu::GraphicsSystem graphics(memory);
  constexpr std::uint32_t ring = 0x0100BC00;
  constexpr std::uint32_t capacity = 8;
  store_be32(memory, ring + 6u * 4u, make_packet_type1(0x60, 0x61));
  store_be32(memory, ring + 7u * 4u, 0x11112222u);
  store_be32(memory, ring + 0u * 4u, 0x33334444u);
  store_be32(memory, ring + 1u * 4u, make_packet_type2());

  const auto capture = graphics.capture_ring(ring, capacity, 6u, 2u);
  assert(capture.source ==
         xenon::gpu::FrontendSubmissionCapture::Source::Ring);
  assert(capture.capacity_dwords == capacity);
  assert(capture.read_index == 6u);
  assert(capture.write_index == 2u);
  assert(capture.resulting_read_index == 2u);
  assert(capture.final_registers.values[0x60] == 0x11112222u);
  assert(capture.final_registers.values[0x61] == 0x33334444u);
  assert(capture.commands.size() == 2u);
}

void test_gpu_unsupported_counters_total() {
  xenon::gpu::GpuUnsupportedCounters counters{};
  counters.unknown_packets = 1;
  counters.unknown_registers = 2;
  counters.unsupported_fetch_formats = 3;
  counters.unsupported_texture_formats = 4;
  counters.unsupported_sampler_behaviors = 5;
  counters.unsupported_shader_instructions = 6;
  counters.unsupported_shader_features = 7;
  counters.unhandled_resolve_modes = 8;
  counters.unhandled_depth_stencil_paths = 9;
  counters.unexpected_ownership_transitions = 10;
  counters.failed_resource_barriers = 11;
  counters.fallback_shader_uses = 12;
  assert(counters.total() == 78u);  // 1+2+...+12
}

void test_truncation_fault(AddressSpace& memory, CommandProcessor& cp) {
  constexpr std::uint32_t base = 0x01008000;
  store_be32(memory, base, make_packet_type0(0x100, 2));
  bool threw = false;
  try {
    cp.execute_buffer(base, 1);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  assert(threw);
}

// Part 7 of the AC6 Runtime Readiness pass ("GPU capability / silent
// fallback audit"): a register index outside RegisterFile::kRegisterCount
// must be observable in Statistics::unknown_register_writes even though
// emit_register_write() still throws for it (a deliberate, pre-existing
// hard-fail this pass does not change - see command_processor.cpp).
void test_unknown_register_write_is_counted(AddressSpace& memory, CommandProcessor& cp) {
  constexpr std::uint32_t base = 0x01009000;
  constexpr std::uint32_t out_of_range_register = RegisterFile::kRegisterCount;
  write_words(memory, base,
             {make_packet_type0(out_of_range_register, 1), 0xDEADBEEFu});
  const auto before = cp.statistics().unknown_register_writes;
  bool threw = false;
  try {
    cp.execute_buffer(base, 2);
  } catch (const std::out_of_range&) {
    threw = true;
  }
  assert(threw && "an out-of-range register index must still fail loudly");
  assert(cp.statistics().unknown_register_writes == before + 1u);
}

}  // namespace

int main() {
  test_headers();
  test_gpu_unsupported_counters_total();

  AddressSpace memory;
  assert(memory.initialize());
  RegisterFile regs;
  xenon::gpu::ir::Stream stream;
  CommandProcessor cp(memory, regs, stream);

  test_register_packets(memory, cp, regs);
  test_constants(memory, cp, regs);
  test_type3_predication(memory, cp, regs, stream);
  test_load_constant_context(memory, cp, regs);
  test_pm4_reg_rmw(memory, cp, regs);
  test_pm4_reg_to_mem(memory, cp, regs);
  test_pm4_reg_to_mem_loop(memory, cp, regs);
  test_pm4_cond_exec(memory, cp, regs);
  test_pm4_cond_write(memory, cp, regs);
  test_pm4_wait_reg_mem(memory, cp);
  test_pm4_compact_register_waits(memory, cp, regs);
  test_load_alu_constant(memory, cp, regs);
  test_event_write_and_fences(memory, cp, regs);
  test_event_extent(memory, cp, regs);
  test_visibility_queries(memory, cp, regs);
  test_interrupt_dispatch(memory, cp);
  test_mem_write(memory, cp);
  test_indirect_buffer(memory, cp, regs);
  test_ring_wrap(memory, cp, regs);
  test_draw_ir(memory, cp, regs, stream);
  test_dma_draw_ir(memory, cp, regs, stream);
  test_binned_draw_ir(memory, cp, regs, stream);
  test_shader_loads(memory, cp, stream);
  test_shader_partition_state(memory, cp);
  test_im_store_round_trip(memory, cp, stream);
  test_shader_invalidation(memory, cp, stream);
  test_draw_shader_bindings(memory, cp, stream);
  test_immediate_draw_ir(memory, cp, stream);
  test_shader_control_flow_unpack();
  test_edram();
  test_graphics_system_backend(memory);
  test_frontend_capture_replay(memory);
  test_portable_capture_round_trip(memory);
  test_frontend_ring_capture(memory);
  test_truncation_fault(memory, cp);
  test_unknown_register_write_is_counted(memory, cp);

  std::cout << "xenon_gpu_frontend_tests: ok (PM4 + shared memory + graphics IR)\n";
  return 0;
}
