#include "xenon/gpu/command_processor.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <string>

namespace xenon::gpu {
namespace {

[[nodiscard]] std::uint32_t load_be32(const std::byte* p) noexcept {
  std::uint32_t v{};
  std::memcpy(&v, p, sizeof(v));
  if constexpr (std::endian::native == std::endian::little) {
    v = ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
        ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
  }
  return v;
}

void store_le32(std::byte* p, std::uint32_t value) noexcept {
  if constexpr (std::endian::native == std::endian::big) {
    value = ((value & 0x000000FFu) << 24) |
            ((value & 0x0000FF00u) << 8) |
            ((value & 0x00FF0000u) >> 8) |
            ((value & 0xFF000000u) >> 24);
  }
  std::memcpy(p, &value, sizeof(value));
}

[[nodiscard]] bool is_draw_opcode(Type3Opcode opcode) noexcept {
  switch (opcode) {
    case Type3Opcode::DrawIndx:
    case Type3Opcode::DrawIndx2:
    case Type3Opcode::DrawIndxBin:
    case Type3Opcode::DrawIndx2Bin:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool is_shader_opcode(Type3Opcode opcode) noexcept {
  switch (opcode) {
    case Type3Opcode::ImLoad:
    case Type3Opcode::ImLoadImmediate:
    case Type3Opcode::ImStore:
    case Type3Opcode::SetShaderBases:
    case Type3Opcode::SetShaderConstants:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool is_sync_opcode(Type3Opcode opcode) noexcept {
  switch (opcode) {
    case Type3Opcode::WaitForIdle:
    case Type3Opcode::WaitRegMem:
    case Type3Opcode::WaitRegEq:
    case Type3Opcode::WaitRegGte:
    case Type3Opcode::WaitUntilRead:
    case Type3Opcode::WaitIndirectBufferPfdComplete:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool is_event_opcode(Type3Opcode opcode) noexcept {
  switch (opcode) {
    case Type3Opcode::VizQuery:
    case Type3Opcode::EventWrite:
    case Type3Opcode::EventWriteShaderDone:
    case Type3Opcode::EventWriteCacheFlush:
    case Type3Opcode::EventWriteExtent:
    case Type3Opcode::EventWriteZPassDone:
    case Type3Opcode::Interrupt:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool is_state_opcode(Type3Opcode opcode) noexcept {
  switch (opcode) {
    case Type3Opcode::RegRmw:
    case Type3Opcode::SetState:
    case Type3Opcode::SetConstant:
    case Type3Opcode::SetConstant2:
    case Type3Opcode::LoadConstantContext:
    case Type3Opcode::LoadAluConstant:
    case Type3Opcode::InvalidateState:
    case Type3Opcode::CondExec:
    case Type3Opcode::CondWrite:
    case Type3Opcode::ContextUpdate:
    case Type3Opcode::SetBinBaseOffset:
    case Type3Opcode::SetBinMask:
    case Type3Opcode::SetBinSelect:
    case Type3Opcode::SetBinMaskLow:
    case Type3Opcode::SetBinMaskHigh:
    case Type3Opcode::SetBinSelectLow:
    case Type3Opcode::SetBinSelectHigh:
    case Type3Opcode::MeInit:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] std::uint32_t constant_register_base(std::uint32_t type) {
  switch (type) {
    case 0: return 0x4000u;  // ALU constants.
    case 1: return 0x4800u;  // Fetch constants.
    case 2: return 0x4900u;  // Boolean constants.
    case 3: return 0x4908u;  // Loop constants.
    case 4: return 0x2000u;  // Ordinary context registers.
    default:
      throw std::runtime_error("unsupported Xenos SET_CONSTANT block type");
  }
}

}  // namespace

class CommandProcessor::Reader {
 public:
  Reader(memory::AddressSpace& memory, std::uint32_t base,
         std::uint32_t capacity_dwords, std::uint32_t start_index,
         std::uint32_t available_dwords)
      : memory_(memory),
        base_(cpu_to_gpu_address(base)),
        capacity_(capacity_dwords),
        index_(capacity_dwords ? start_index % capacity_dwords : 0),
        remaining_(available_dwords) {
    if (!capacity_) throw std::invalid_argument("zero-sized GPU command buffer");
    const std::uint64_t bytes = std::uint64_t(capacity_) * 4u;
    if (std::uint64_t(base_) + bytes > memory::kPhysicalMemorySize) {
      throw std::out_of_range("GPU command buffer outside physical RAM");
    }
  }

  [[nodiscard]] std::uint32_t read() {
    if (!remaining_) throw std::runtime_error("truncated GPU command packet");
    const std::uint32_t address = base_ + index_ * 4u;
    const auto* ptr = memory_.physical_data(address);
    if (!ptr) throw std::runtime_error("GPU command read from invalid physical RAM");
    const std::uint32_t value = load_be32(ptr);
    index_ = (index_ + 1u) % capacity_;
    --remaining_;
    return value;
  }

  [[nodiscard]] std::uint32_t remaining() const noexcept { return remaining_; }
  [[nodiscard]] std::uint32_t index() const noexcept { return index_; }

 private:
  memory::AddressSpace& memory_;
  std::uint32_t base_{};
  std::uint32_t capacity_{};
  std::uint32_t index_{};
  std::uint32_t remaining_{};
};

CommandProcessor::CommandProcessor(memory::AddressSpace& memory,
                                   RegisterFile& registers, ir::Stream& stream)
    : memory_(memory), registers_(registers), stream_(stream) {}

void CommandProcessor::reset() {
  registers_.reset();
  stream_.clear();
  stats_ = {};
  submission_dwords_ = 0;
  max_indirect_depth_ = 0;
}

void CommandProcessor::execute_buffer(std::uint32_t physical_address,
                                      std::uint32_t dword_count) {
  submission_dwords_ = 0;
  max_indirect_depth_ = 0;
  execute_buffer_internal(physical_address, dword_count, 0);
}

std::uint32_t CommandProcessor::execute_ring(std::uint32_t physical_address,
                                             std::uint32_t capacity_dwords,
                                             std::uint32_t read_index,
                                             std::uint32_t write_index) {
  if (!capacity_dwords) throw std::invalid_argument("zero-sized GPU ring");
  read_index %= capacity_dwords;
  write_index %= capacity_dwords;
  const std::uint32_t available = write_index >= read_index
                                      ? write_index - read_index
                                      : capacity_dwords - read_index + write_index;
  submission_dwords_ = 0;
  max_indirect_depth_ = 0;
  Reader reader(memory_, physical_address, capacity_dwords, read_index, available);
  while (reader.remaining()) execute_packet(reader, 0);
  return reader.index();
}

void CommandProcessor::execute_buffer_internal(std::uint32_t physical_address,
                                               std::uint32_t dword_count,
                                               std::uint32_t depth) {
  if (depth > kMaximumIndirectDepth) {
    throw std::runtime_error("Xenos indirect-buffer recursion limit exceeded");
  }
  max_indirect_depth_ = std::max(max_indirect_depth_, depth);
  if (submission_dwords_ + dword_count > kMaximumDwordsPerSubmission) {
    throw std::runtime_error("Xenos command submission exceeds safety limit");
  }
  submission_dwords_ += dword_count;
  Reader reader(memory_, physical_address, dword_count, 0, dword_count);
  while (reader.remaining()) execute_packet(reader, depth);
}

void CommandProcessor::execute_packet(Reader& reader, std::uint32_t depth) {
  const auto header = decode_packet_header(reader.read());
  ++stats_.packets;
  switch (header.type) {
    case PacketType::Type0:
      ++stats_.type0_packets;
      execute_type0(reader, header);
      return;
    case PacketType::Type1:
      ++stats_.type1_packets;
      execute_type1(reader, header);
      return;
    case PacketType::Type2:
      ++stats_.type2_packets;
      return;
    case PacketType::Type3:
      ++stats_.type3_packets;
      execute_type3(reader, header, depth);
      return;
  }
  throw std::runtime_error("invalid Xenos packet type");
}

void CommandProcessor::execute_type0(Reader& reader, const PacketHeader& header) {
  for (std::uint32_t i = 0; i < header.count; ++i) {
    const std::uint32_t index = header.write_one_register
                                    ? header.register_index
                                    : std::uint32_t(header.register_index) + i;
    emit_register_write(index, reader.read());
  }
}

void CommandProcessor::execute_type1(Reader& reader, const PacketHeader& header) {
  emit_register_write(header.register_index_1, reader.read());
  emit_register_write(header.register_index_2, reader.read());
}

std::vector<std::uint32_t> CommandProcessor::read_payload(Reader& reader,
                                                          std::uint32_t count) {
  if (reader.remaining() < count) {
    throw std::runtime_error("truncated Xenos type-3 packet");
  }
  std::vector<std::uint32_t> payload;
  payload.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) payload.push_back(reader.read());
  return payload;
}

void CommandProcessor::emit_register_write(std::uint32_t index,
                                           std::uint32_t value) {
  if (!registers_.write(index, value)) {
    throw std::out_of_range("Xenos register index outside register file");
  }
  stream_.emit(ir::RegisterWrite{index, value});
}

void CommandProcessor::write_physical_dword(std::uint32_t address_with_endian,
                                            std::uint32_t logical_value) {
  const Endian endian = static_cast<Endian>(address_with_endian & 0x3u);
  const std::uint32_t address = cpu_to_gpu_address(address_with_endian & ~0x3u);
  if (std::uint64_t(address) + 4u > memory::kPhysicalMemorySize) {
    throw std::out_of_range("Xenos physical write outside RAM");
  }
  const std::uint32_t stored = gpu_swap(logical_value, endian);
  auto* p = memory_.physical_data(address);
  if (!p) throw std::runtime_error("Xenos physical write has no backing");
  // The Xenos memory path is little-endian before the packet-selected endian
  // transformation; CPU-visible big-endian access is supplied by MemoryPort.
  store_le32(p, stored);
  memory_.notify_external_write(address, 4);
  stream_.emit(ir::PhysicalMemoryWrite{address, logical_value, endian});
  ++stats_.physical_writes;
}

void CommandProcessor::execute_mem_write(std::span<const std::uint32_t> payload) {
  if (payload.empty()) throw std::runtime_error("PM4_MEM_WRITE missing address");
  std::uint32_t address = payload[0];
  for (std::size_t i = 1; i < payload.size(); ++i, address += 4u) {
    write_physical_dword(address, payload[i]);
  }
}

void CommandProcessor::execute_type3(Reader& reader, const PacketHeader& header,
                                     std::uint32_t depth) {
  auto payload = read_payload(reader, header.count);

  if (header.opcode == Type3Opcode::Nop) {
    // NOP payloads often carry debug strings/markers. Preserve them in the IR
    // rather than treating the bytes as semantic state.
    stream_.emit(ir::Type3Packet{header.opcode, header.predicate, std::move(payload)});
    return;
  }

  if (header.opcode == Type3Opcode::IndirectBuffer ||
      header.opcode == Type3Opcode::IndirectBufferPfd) {
    if (payload.size() < 2) {
      throw std::runtime_error("PM4_INDIRECT_BUFFER requires address and length");
    }
    const std::uint32_t address = cpu_to_gpu_address(payload[0]);
    const std::uint32_t length = payload[1] & 0xFFFFFu;
    const bool prefetch = header.opcode == Type3Opcode::IndirectBufferPfd;
    stream_.emit(ir::IndirectBuffer{address, length, prefetch});
    ++stats_.indirect_buffers;
    if (length) execute_buffer_internal(address, length, depth + 1u);
    return;
  }

  if (header.opcode == Type3Opcode::MemWrite) {
    execute_mem_write(payload);
    return;
  }

  if (header.opcode == Type3Opcode::SetConstant) {
    if (payload.empty()) throw std::runtime_error("PM4_SET_CONSTANT missing selector");
    std::uint32_t index = (payload[0] & 0x7FFu) +
                          constant_register_base((payload[0] >> 16) & 0xFFu);
    for (std::size_t i = 1; i < payload.size(); ++i) {
      emit_register_write(index++, payload[i]);
    }
    stream_.emit(ir::StatePacket{header.opcode, payload});
    return;
  }

  if (header.opcode == Type3Opcode::SetConstant2 ||
      header.opcode == Type3Opcode::SetShaderConstants) {
    if (payload.empty()) throw std::runtime_error("constant packet missing selector");
    std::uint32_t index = payload[0] & 0xFFFFu;
    for (std::size_t i = 1; i < payload.size(); ++i) {
      emit_register_write(index++, payload[i]);
    }
    if (header.opcode == Type3Opcode::SetShaderConstants) {
      stream_.emit(ir::ShaderPacket{header.opcode, payload});
    } else {
      stream_.emit(ir::StatePacket{header.opcode, payload});
    }
    return;
  }

  if (header.opcode == Type3Opcode::ImLoad) {
    if (payload.size() < 2) {
      throw std::runtime_error("PM4_IM_LOAD requires address/type and start/size");
    }
    const auto stage_bits = payload[0] & 0x3u;
    if (stage_bits > 1u) throw std::runtime_error("invalid Xenos shader stage");
    const auto stage = stage_bits == 0 ? ShaderStage::Vertex : ShaderStage::Pixel;
    const std::uint32_t address = cpu_to_gpu_address(payload[0] & ~0x3u);
    const std::uint32_t start = payload[1] >> 16;
    const std::uint32_t size = payload[1] & 0xFFFFu;
    if (std::uint64_t(address) + std::uint64_t(size) * 4u > memory::kPhysicalMemorySize) {
      throw std::out_of_range("PM4_IM_LOAD shader outside physical RAM");
    }
    std::vector<std::uint32_t> microcode;
    microcode.reserve(size);
    for (std::uint32_t i = 0; i < size; ++i) {
      microcode.push_back(load_be32(memory_.physical_data(address + i * 4u)));
    }
    stream_.emit(ir::ShaderLoad{header.opcode, false, address,
                                ShaderProgram(stage, microcode, start), payload});
    return;
  }

  if (header.opcode == Type3Opcode::ImLoadImmediate) {
    if (payload.size() < 2) {
      throw std::runtime_error("PM4_IM_LOAD_IMMEDIATE requires stage and start/size");
    }
    const auto stage_bits = payload[0] & 0x3u;
    if (stage_bits > 1u) throw std::runtime_error("invalid Xenos shader stage");
    const auto stage = stage_bits == 0 ? ShaderStage::Vertex : ShaderStage::Pixel;
    const std::uint32_t start = payload[1] >> 16;
    const std::uint32_t size = payload[1] & 0xFFFFu;
    if (payload.size() < std::size_t(2u + size)) {
      throw std::runtime_error("truncated PM4_IM_LOAD_IMMEDIATE microcode");
    }
    const std::span<const std::uint32_t> microcode(payload.data() + 2, size);
    stream_.emit(ir::ShaderLoad{header.opcode, true, 0,
                                ShaderProgram(stage, microcode, start), payload});
    return;
  }

  if (header.opcode == Type3Opcode::LoadConstantContext) {
    if (payload.size() < 3) {
      throw std::runtime_error("PM4_LOAD_CONSTANT_CONTEXT requires 3 dwords");
    }
    const std::uint32_t address = cpu_to_gpu_address(payload[0] & 0x3FFFFFFFu);
    std::uint32_t index = (payload[1] & 0x7FFu) +
                          constant_register_base((payload[1] >> 16) & 0xFFu);
    const std::uint32_t size = payload[2] & 0xFFFu;
    if (std::uint64_t(address) + std::uint64_t(size) * 4u > memory::kPhysicalMemorySize) {
      throw std::out_of_range("PM4_LOAD_CONSTANT_CONTEXT outside physical RAM");
    }
    for (std::uint32_t i = 0; i < size; ++i) {
      emit_register_write(index++, load_be32(memory_.physical_data(address + i * 4u)));
    }
    stream_.emit(ir::StatePacket{header.opcode, payload});
    return;
  }

  if (is_draw_opcode(header.opcode)) {
    ++stats_.draws;
    stream_.emit(ir::DrawPacket{header.opcode, header.predicate,
                                registers_.generation(), std::move(payload)});
    return;
  }

  if (is_shader_opcode(header.opcode)) {
    stream_.emit(ir::ShaderPacket{header.opcode, std::move(payload)});
    return;
  }
  if (is_sync_opcode(header.opcode)) {
    stream_.emit(ir::SynchronizationPacket{header.opcode, std::move(payload)});
    return;
  }
  if (is_event_opcode(header.opcode)) {
    stream_.emit(ir::EventPacket{header.opcode, std::move(payload)});
    return;
  }
  if (is_state_opcode(header.opcode)) {
    stream_.emit(ir::StatePacket{header.opcode, std::move(payload)});
    return;
  }

  // Unknown/undocumented type-3 values are retained exactly. This is not a
  // runtime emulation fallback: it is lossless frontend IR so a later analysis
  // or backend can diagnose/implement the command without corrupting the stream.
  stream_.emit(ir::Type3Packet{header.opcode, header.predicate, std::move(payload)});
}

}  // namespace xenon::gpu
