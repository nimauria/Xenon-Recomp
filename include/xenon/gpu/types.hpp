#pragma once

#include <bit>
#include <cstdint>
#include <stdexcept>

namespace xenon::gpu {

enum class PacketType : std::uint8_t {
  Type0 = 0,
  Type1 = 1,
  Type2 = 2,
  Type3 = 3,
};

enum class ShaderStage : std::uint8_t {
  Vertex,
  Pixel,
};

enum class PrimitiveType : std::uint8_t {
  None = 0x00,
  PointList = 0x01,
  LineList = 0x02,
  LineStrip = 0x03,
  TriangleList = 0x04,
  TriangleFan = 0x05,
  TriangleStrip = 0x06,
  TriangleWithWFlags = 0x07,
  RectangleList = 0x08,
  LineLoop = 0x0C,
  QuadList = 0x0D,
  QuadStrip = 0x0E,
  Polygon = 0x0F,
  CopyRectList0 = 0x10,
  CopyRectList1 = 0x11,
  CopyRectList2 = 0x12,
  CopyRectList3 = 0x13,
  FillRectList = 0x14,
  TwoDLineStrip = 0x15,
  TwoDTriStrip = 0x16,
};

enum class Endian : std::uint8_t {
  None = 0,
  Swap8In16 = 1,
  Swap8In32 = 2,
  Swap16In32 = 3,
};

// Xenos type-3 PM4 opcodes. These are command-processor operations, not host
// backend operations; the graphics backend never consumes PM4 directly.
enum class Type3Opcode : std::uint8_t {
  Nop = 0x10,
  RegRmw = 0x21,
  DrawIndx = 0x22,
  VizQuery = 0x23,
  SetState = 0x25,
  WaitForIdle = 0x26,
  ImLoad = 0x27,
  ImLoadImmediate = 0x2B,
  ImStore = 0x2C,
  SetConstant = 0x2D,
  LoadConstantContext = 0x2E,
  LoadAluConstant = 0x2F,
  DrawIndxBin = 0x34,
  DrawIndx2Bin = 0x35,
  DrawIndx2 = 0x36,
  IndirectBufferPfd = 0x37,
  InvalidateState = 0x3B,
  WaitRegMem = 0x3C,
  MemWrite = 0x3D,
  RegToMem = 0x3E,
  IndirectBuffer = 0x3F,
  CondExec = 0x44,
  CondWrite = 0x45,
  EventWrite = 0x46,
  MeInit = 0x48,
  SetShaderBases = 0x4A,
  SetBinBaseOffset = 0x4B,
  MemWriteCounter = 0x4F,
  SetBinMask = 0x50,
  SetBinSelect = 0x51,
  WaitRegEq = 0x52,
  WaitRegGte = 0x53,
  Interrupt = 0x54,
  SetConstant2 = 0x55,
  SetShaderConstants = 0x56,
  EventWriteShaderDone = 0x58,
  EventWriteCacheFlush = 0x59,
  EventWriteExtent = 0x5A,
  EventWriteZPassDone = 0x5B,
  WaitUntilRead = 0x5C,
  WaitIndirectBufferPfdComplete = 0x5D,
  ContextUpdate = 0x5E,
  SetBinMaskLow = 0x60,
  SetBinMaskHigh = 0x61,
  SetBinSelectLow = 0x62,
  SetBinSelectHigh = 0x63,
  // 0x64 is a host-emulator extension in Xenia and intentionally is not part
  // of the Xenon hardware opcode catalogue.
};

struct PacketHeader {
  PacketType type{};
  std::uint32_t raw{};
  std::uint16_t count{};  // Payload dwords for type 0/3; 2 for type 1, 0 for type 2.
  std::uint16_t register_index{};
  bool write_one_register{};
  std::uint16_t register_index_1{};
  std::uint16_t register_index_2{};
  Type3Opcode opcode{};
  bool predicate{};
};

[[nodiscard]] constexpr PacketHeader decode_packet_header(std::uint32_t word) noexcept {
  PacketHeader result{};
  result.raw = word;
  result.type = static_cast<PacketType>(word >> 30);
  switch (result.type) {
    case PacketType::Type0:
      result.count = static_cast<std::uint16_t>(((word >> 16) & 0x3FFFu) + 1u);
      result.register_index = static_cast<std::uint16_t>(word & 0x7FFFu);
      result.write_one_register = ((word >> 15) & 1u) != 0;
      break;
    case PacketType::Type1:
      result.count = 2;
      result.register_index_1 = static_cast<std::uint16_t>(word & 0x7FFu);
      result.register_index_2 = static_cast<std::uint16_t>((word >> 11) & 0x7FFu);
      break;
    case PacketType::Type2:
      result.count = 0;
      break;
    case PacketType::Type3:
      result.count = static_cast<std::uint16_t>(((word >> 16) & 0x3FFFu) + 1u);
      result.opcode = static_cast<Type3Opcode>((word >> 8) & 0x7Fu);
      result.predicate = (word & 1u) != 0;
      break;
  }
  return result;
}

[[nodiscard]] constexpr std::uint32_t make_packet_type0(
    std::uint16_t index, std::uint16_t count, bool one_register = false) {
  if (index > 0x7FFFu || count == 0 || count > 0x4000u) {
    throw std::out_of_range("invalid PM4 type-0 packet");
  }
  return (std::uint32_t(count - 1u) << 16) |
         (std::uint32_t(one_register) << 15) | index;
}

[[nodiscard]] constexpr std::uint32_t make_packet_type1(
    std::uint16_t index1, std::uint16_t index2) {
  if (index1 > 0x7FFu || index2 > 0x7FFu) {
    throw std::out_of_range("invalid PM4 type-1 packet");
  }
  return (1u << 30) | (std::uint32_t(index2) << 11) | index1;
}

[[nodiscard]] constexpr std::uint32_t make_packet_type2() noexcept {
  return 2u << 30;
}

[[nodiscard]] constexpr std::uint32_t make_packet_type3(
    Type3Opcode opcode, std::uint16_t count, bool predicate = false) {
  if (count == 0 || count > 0x4000u) {
    throw std::out_of_range("invalid PM4 type-3 packet");
  }
  return (3u << 30) | (std::uint32_t(count - 1u) << 16) |
         (std::uint32_t(opcode) << 8) | std::uint32_t(predicate);
}

[[nodiscard]] constexpr std::uint32_t cpu_to_gpu_address(std::uint32_t address) noexcept {
  return address & 0x1FFFFFFFu;
}

[[nodiscard]] constexpr std::uint32_t gpu_to_cpu_address(std::uint32_t address) noexcept {
  return address;
}

[[nodiscard]] constexpr std::uint32_t gpu_swap(std::uint32_t value,
                                                Endian endian) noexcept {
  switch (endian) {
    case Endian::None:
      return value;
    case Endian::Swap8In16:
      return ((value & 0x00FF00FFu) << 8) | ((value & 0xFF00FF00u) >> 8);
    case Endian::Swap8In32:
      return ((value & 0x000000FFu) << 24) |
             ((value & 0x0000FF00u) << 8) |
             ((value & 0x00FF0000u) >> 8) |
             ((value & 0xFF000000u) >> 24);
    case Endian::Swap16In32:
      return (value << 16) | (value >> 16);
  }
  return value;
}

}  // namespace xenon::gpu
