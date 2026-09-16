#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <variant>

#include "xenon/gpu/command_processor.hpp"
#include "xenon/gpu/graphics_system.hpp"
#include "xenon/gpu/types.hpp"
#include "xenon/memory/address_space.hpp"

using xenon::gpu::CommandProcessor;
using xenon::gpu::Endian;
using xenon::gpu::PacketType;
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
  auto* p = memory.physical_data(physical_address);
  assert(p);
  const std::uint8_t bytes[4] = {
      static_cast<std::uint8_t>(value >> 24),
      static_cast<std::uint8_t>(value >> 16),
      static_cast<std::uint8_t>(value >> 8),
      static_cast<std::uint8_t>(value),
  };
  std::memcpy(p, bytes, sizeof(bytes));
  memory.notify_external_write(physical_address, 4);
}

void write_words(AddressSpace& memory, std::uint32_t base,
                 std::initializer_list<std::uint32_t> words) {
  std::uint32_t address = base;
  for (auto word : words) {
    store_be32(memory, address, word);
    address += 4;
  }
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

void test_mem_write(AddressSpace& memory, CommandProcessor& cp) {
  constexpr std::uint32_t commands = 0x01003000;
  constexpr std::uint32_t target = 0x01200000;
  write_words(memory, commands,
              {make_packet_type3(Type3Opcode::MemWrite, 3),
               target | static_cast<std::uint32_t>(Endian::Swap8In32),
               0x11223344, 0xA1B2C3D4});
  cp.execute_buffer(commands, 4);
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
                  xenon::gpu::ir::Stream& stream) {
  constexpr std::uint32_t base = 0x01007000;
  write_words(memory, base,
              {make_packet_type3(Type3Opcode::DrawIndx2, 2),
               0x00000004, 0x00000003});
  const auto before = stream.size();
  cp.execute_buffer(base, 3);
  assert(cp.statistics().draws >= 1);
  assert(stream.size() == before + 1);
  const auto& command = stream.commands().back();
  assert(std::holds_alternative<xenon::gpu::ir::DrawPacket>(command));
  const auto& draw = std::get<xenon::gpu::ir::DrawPacket>(command);
  assert(draw.opcode == Type3Opcode::DrawIndx2);
  assert(draw.payload.size() == 2);
  assert(draw.payload[0] == 4);
  assert(draw.payload[1] == 3);
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
  write_words(memory, base,
              {make_packet_type0(0x44, 1), 0x12345678,
               make_packet_type3(Type3Opcode::DrawIndx2, 2), 4, 6});
  graphics.submit_buffer(base, 5);
  xenon::gpu::NullBackend backend;
  const auto pending = graphics.stream().size();
  graphics.execute_ir(backend);
  assert(backend.command_count() == pending);
  assert(backend.command_count() == 2);
  assert(graphics.stream().size() == 0);
  assert(graphics.registers().read(0x44) == 0x12345678);
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

}  // namespace

int main() {
  test_headers();

  AddressSpace memory;
  assert(memory.initialize());
  RegisterFile regs;
  xenon::gpu::ir::Stream stream;
  CommandProcessor cp(memory, regs, stream);

  test_register_packets(memory, cp, regs);
  test_constants(memory, cp, regs);
  test_load_constant_context(memory, cp, regs);
  test_mem_write(memory, cp);
  test_indirect_buffer(memory, cp, regs);
  test_ring_wrap(memory, cp, regs);
  test_draw_ir(memory, cp, stream);
  test_shader_loads(memory, cp, stream);
  test_shader_control_flow_unpack();
  test_edram();
  test_graphics_system_backend(memory);
  test_truncation_fault(memory, cp);

  std::cout << "xenon_gpu_frontend_tests: ok (PM4 + shared memory + graphics IR)\n";
  return 0;
}
