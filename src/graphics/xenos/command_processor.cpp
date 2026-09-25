#include "xenon/gpu/command_processor.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

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

[[nodiscard]] std::array<std::byte, 4> encode_be32(
    std::uint32_t value) noexcept {
  return {static_cast<std::byte>(value >> 24),
          static_cast<std::byte>(value >> 16),
          static_cast<std::byte>(value >> 8),
          static_cast<std::byte>(value)};
}

[[nodiscard]] std::array<std::byte, 2> encode_be16(
    std::uint16_t value) noexcept {
  return {static_cast<std::byte>(value >> 8),
          static_cast<std::byte>(value)};
}

[[nodiscard]] std::array<std::byte, 4> encode_le32(
    std::uint32_t value) noexcept {
  if constexpr (std::endian::native == std::endian::big) {
    value = ((value & 0x000000FFu) << 24) |
            ((value & 0x0000FF00u) << 8) |
            ((value & 0x00FF0000u) >> 8) |
            ((value & 0xFF000000u) >> 24);
  }
  std::array<std::byte, 4> bytes{};
  std::memcpy(bytes.data(), &value, sizeof(value));
  return bytes;
}

[[nodiscard]] std::uint32_t load_le32(const std::byte* p) noexcept {
  std::uint32_t v{};
  std::memcpy(&v, p, sizeof(v));
  if constexpr (std::endian::native == std::endian::big) {
    v = ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
        ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24);
  }
  return v;
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

// Xenos context registers written implicitly by DRAW_INDX packet fields.
// Keeping these writes in the frontend means every host backend observes the
// same register stream without needing to know the PM4 packet layout.
constexpr std::uint32_t kVgtDmaBase = 0x21FAu;
constexpr std::uint32_t kVgtDmaSize = 0x21FBu;
constexpr std::uint32_t kVgtDrawInitiator = 0x21FCu;
constexpr std::uint32_t kVgtEventInitiator = 0x21F9u;
constexpr std::uint32_t kPaScVizQueryStatus0 = 0x0C44u;
constexpr std::uint32_t kPaScVizQueryStatus1 = 0x0C45u;
constexpr std::uint32_t kTexture2DCubeMaxWidthHeight = 8192u;

struct DrawInitiatorFields {
  PrimitiveType primitive{PrimitiveType::None};
  DrawSource source{DrawSource::Reserved};
  MajorMode major_mode{MajorMode::Implicit};
  IndexFormat index_format{IndexFormat::UInt16};
  bool not_eop{};
  std::uint32_t index_count{};
};

[[nodiscard]] DrawInitiatorFields decode_draw_initiator(
    std::uint32_t value) noexcept {
  DrawInitiatorFields fields{};
  fields.primitive = static_cast<PrimitiveType>(value & 0x3Fu);
  fields.source = static_cast<DrawSource>((value >> 6) & 0x3u);
  fields.major_mode = static_cast<MajorMode>((value >> 8) & 0x3u);
  fields.index_format = static_cast<IndexFormat>((value >> 11) & 0x1u);
  fields.not_eop = ((value >> 12) & 0x1u) != 0;
  fields.index_count = value >> 16;
  return fields;
}

[[nodiscard]] std::uint32_t index_size_bytes(IndexFormat format) noexcept {
  return format == IndexFormat::UInt32 ? 4u : 2u;
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
    std::array<std::byte, 4> snapshot{};
    if (!memory_.copy_physical_range(address, snapshot)) {
      throw std::runtime_error("GPU command read from invalid physical RAM");
    }
    const std::uint32_t value = load_be32(snapshot.data());
    index_ = (index_ + 1u) % capacity_;
    --remaining_;
    return value;
  }

  void skip(std::uint32_t dwords) {
    if (dwords > remaining_) {
      throw std::runtime_error("truncated PM4 conditional execution block");
    }
    index_ = (index_ + dwords) % capacity_;
    remaining_ -= dwords;
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
  active_vertex_shader_ = {};
  active_pixel_shader_ = {};
  active_vertex_program_.reset();
  active_pixel_program_.reset();
  shader_partition_ = {};
  bin_base_offset_ = 0;
  bin_mask_ = 0;
  bin_select_ = 0;
  active_viz_queries_ = 0;
  viz_query_draws_ = 0;
  swap_counter_ = 0;
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
    ++stats_.unknown_register_writes;
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
  // The Xenos memory path is little-endian before the packet-selected endian
  // transformation; CPU-visible big-endian access is supplied by MemoryPort.
  const auto bytes = encode_le32(stored);
  if (!memory_.write_physical(address, bytes)) {
    throw std::runtime_error("Xenos physical write has no backing");
  }
  stream_.emit(ir::PhysicalMemoryWrite{address, logical_value, endian});
  ++stats_.physical_writes;
}

std::uint32_t CommandProcessor::read_physical_dword(
    std::uint32_t address_with_endian) const {
  const Endian endian = static_cast<Endian>(address_with_endian & 0x3u);
  const std::uint32_t address =
      cpu_to_gpu_address(address_with_endian & ~0x3u);
  if (std::uint64_t(address) + 4u > memory::kPhysicalMemorySize) {
    throw std::out_of_range("Xenos physical read outside RAM");
  }
  std::array<std::byte, 4> snapshot{};
  if (!memory_.copy_physical_range(address, snapshot)) {
    throw std::runtime_error("Xenos physical read has no backing");
  }
  return gpu_swap(load_le32(snapshot.data()), endian);
}

bool CommandProcessor::wait_condition_matches(std::uint32_t wait_info,
                                              std::uint32_t value,
                                              std::uint32_t reference,
                                              std::uint32_t mask) noexcept {
  const std::uint32_t masked = value & mask;
  switch (wait_info & 0x7u) {
    case 0x0u: return false;
    case 0x1u: return masked < reference;
    case 0x2u: return masked <= reference;
    case 0x3u: return masked == reference;
    case 0x4u: return masked != reference;
    case 0x5u: return masked >= reference;
    case 0x6u: return masked > reference;
    case 0x7u: return true;
  }
  return false;
}

void CommandProcessor::execute_reg_rmw(
    std::span<const std::uint32_t> payload) {
  if (payload.size() != 3u) {
    throw std::runtime_error("PM4_REG_RMW requires exactly 3 dwords");
  }

  const std::uint32_t rmw_info = payload[0];
  const std::uint32_t target = rmw_info & 0x1FFFu;
  if (!registers_.valid(target)) {
    throw std::out_of_range("PM4_REG_RMW target register outside register file");
  }

  std::uint32_t value = registers_.read(target);
  if ((rmw_info & 0x80000000u) != 0u) {
    const std::uint32_t source = payload[1] & 0x1FFFu;
    if (!registers_.valid(source)) {
      throw std::out_of_range("PM4_REG_RMW AND register outside register file");
    }
    value &= registers_.read(source);
  } else {
    value &= payload[1];
  }

  if ((rmw_info & 0x40000000u) != 0u) {
    const std::uint32_t source = payload[2] & 0x1FFFu;
    if (!registers_.valid(source)) {
      throw std::out_of_range("PM4_REG_RMW OR register outside register file");
    }
    value |= registers_.read(source);
  } else {
    value |= payload[2];
  }

  emit_register_write(target, value);
  ++stats_.register_rmw_packets;
}

void CommandProcessor::execute_reg_to_mem(
    std::span<const std::uint32_t> payload) {
  if (payload.size() != 2u) {
    throw std::runtime_error("PM4_REG_TO_MEM requires exactly 2 dwords");
  }

  // A2xx/Xenos-family REG_TO_MEM packs the source register in bits 0..17
  // and a repeat count in bits 18..29. A zero count still transfers one
  // dword. The 64-byte and accumulate forms are distinct transfer modes and
  // are intentionally rejected until their Xenos-specific behavior is needed.
  const std::uint32_t source_info = payload[0];
  if ((source_info & 0xC0000000u) != 0u) {
    throw std::runtime_error(
        "PM4_REG_TO_MEM 64B/ACCUMULATE mode is not supported yet");
  }
  const std::uint32_t first_register = source_info & 0x3FFFFu;
  const std::uint32_t encoded_count = (source_info >> 18) & 0xFFFu;
  const std::uint32_t transfer_count = std::max(1u, encoded_count);
  if (!registers_.valid(first_register) ||
      std::uint64_t(first_register) + transfer_count >
          RegisterFile::kRegisterCount) {
    throw std::out_of_range(
        "PM4_REG_TO_MEM source range outside register file");
  }

  const std::uint32_t endian_bits = payload[1] & 0x3u;
  const std::uint32_t destination = payload[1] & ~0x3u;
  if (std::uint64_t(cpu_to_gpu_address(destination)) +
          std::uint64_t(transfer_count) * 4u >
      memory::kPhysicalMemorySize) {
    throw std::out_of_range("PM4_REG_TO_MEM destination outside physical RAM");
  }
  for (std::uint32_t i = 0; i < transfer_count; ++i) {
    write_physical_dword((destination + i * 4u) | endian_bits,
                         registers_.read(first_register + i));
  }
  ++stats_.register_to_memory_packets;
  stats_.register_to_memory_dwords += transfer_count;
}

void CommandProcessor::execute_cond_exec(
    Reader& reader, std::span<const std::uint32_t> payload) {
  if (payload.size() != 4u) {
    throw std::runtime_error("PM4_COND_EXEC requires exactly 4 dwords");
  }

  // The Xenos-era CP_COND_EXEC body stores two physical addresses in dword
  // units, followed by a signed reference and the number of subsequent command
  // dwords controlled by the condition. Execute when *ADDR0 != 0 and
  // int32(*ADDR1) < int32(REF).
  const auto decode_dword_address = [](std::uint32_t encoded) {
    const std::uint64_t byte_address = std::uint64_t(encoded) << 2u;
    if (byte_address > UINT32_MAX) {
      throw std::out_of_range("PM4_COND_EXEC address overflow");
    }
    return cpu_to_gpu_address(static_cast<std::uint32_t>(byte_address));
  };
  const std::uint32_t address0 = decode_dword_address(payload[0]);
  const std::uint32_t address1 = decode_dword_address(payload[1]);
  const std::uint32_t value0 = read_physical_dword(address0);
  const std::uint32_t value1 = read_physical_dword(address1);
  const bool execute =
      value0 != 0u &&
      static_cast<std::int32_t>(value1) <
          static_cast<std::int32_t>(payload[2]);

  ++stats_.conditional_exec_packets;
  if (execute) {
    ++stats_.conditional_exec_taken;
    return;
  }
  reader.skip(payload[3]);
  stats_.conditional_exec_dwords_skipped += payload[3];
}

void CommandProcessor::execute_cond_write(
    std::span<const std::uint32_t> payload) {
  if (payload.size() != 6u) {
    throw std::runtime_error("PM4_COND_WRITE requires exactly 6 dwords");
  }

  const std::uint32_t wait_info = payload[0];
  std::uint32_t value{};
  if ((wait_info & 0x10u) != 0u) {
    value = read_physical_dword(payload[1]);
  } else {
    if (!registers_.valid(payload[1])) {
      throw std::out_of_range("PM4_COND_WRITE poll register outside register file");
    }
    value = registers_.read(payload[1]);
  }

  ++stats_.conditional_write_packets;
  if (!wait_condition_matches(wait_info, value, payload[2], payload[3])) {
    return;
  }

  if ((wait_info & 0x100u) != 0u) {
    write_physical_dword(payload[4], payload[5]);
  } else {
    emit_register_write(payload[4], payload[5]);
  }
  ++stats_.conditional_writes_taken;
}

void CommandProcessor::execute_wait_reg_mem(
    std::span<const std::uint32_t> payload) {
  if (payload.size() != 5u) {
    throw std::runtime_error("PM4_WAIT_REG_MEM requires exactly 5 dwords");
  }

  const std::uint32_t wait_info = payload[0];
  const bool memory_source = (wait_info & 0x10u) != 0u;
  if (!memory_source && !registers_.valid(payload[1])) {
    throw std::out_of_range("PM4_WAIT_REG_MEM poll register outside register file");
  }

  ++stats_.wait_reg_mem_packets;
  for (std::uint32_t poll = 0; poll < kMaximumWaitPollIterations; ++poll) {
    const std::uint32_t value =
        memory_source ? read_physical_dword(payload[1])
                      : registers_.read(payload[1]);
    ++stats_.wait_reg_mem_polls;
    if (wait_condition_matches(wait_info, value, payload[2], payload[3])) {
      return;
    }

    // WAIT_REG_MEM is a real hardware wait. Yield periodically so a concurrently
    // running recompiled CPU thread or asynchronous producer can satisfy a
    // memory condition, but retain an upper safety bound so corrupt captures do
    // not deadlock the host process forever.
    if ((poll & 0x3Fu) == 0x3Fu) std::this_thread::yield();
  }
  throw std::runtime_error("PM4_WAIT_REG_MEM exceeded host safety poll limit");
}

void CommandProcessor::execute_wait_register(
    std::span<const std::uint32_t> payload, bool greater_or_equal) {
  // The compact WAIT_REG_EQ / WAIT_REG_GTE packet family uses the same
  // four-dword body used by the contemporary A2xx command processor:
  // register, reference, mask, poll interval. Xenos shares these opcodes and
  // packet descriptions. The interval is a hardware scheduling hint; on the
  // host we periodically yield instead of attempting to model CP clock cycles.
  if (payload.size() != 4u) {
    throw std::runtime_error(
        "PM4_WAIT_REG_EQ/GTE requires exactly 4 dwords");
  }
  const std::uint32_t register_index = payload[0];
  if (!registers_.valid(register_index)) {
    throw std::out_of_range("PM4 compact wait register outside register file");
  }

  ++stats_.wait_register_packets;
  for (std::uint32_t poll = 0; poll < kMaximumWaitPollIterations; ++poll) {
    const std::uint32_t value = registers_.read(register_index) & payload[2];
    ++stats_.wait_register_polls;
    if (greater_or_equal ? value >= payload[1] : value == payload[1]) {
      return;
    }
    if ((poll & 0x3Fu) == 0x3Fu) std::this_thread::yield();
  }
  throw std::runtime_error(
      "PM4_WAIT_REG_EQ/GTE exceeded host safety poll limit");
}

void CommandProcessor::execute_load_alu_constant(
    std::span<const std::uint32_t> payload) {
  if (payload.size() != 3u) {
    throw std::runtime_error("PM4_LOAD_ALU_CONSTANT requires exactly 3 dwords");
  }

  const std::uint32_t address =
      cpu_to_gpu_address(payload[0] & 0x3FFFFFFFu);
  std::uint32_t index = (payload[1] & 0x7FFu) +
                        constant_register_base((payload[1] >> 16) & 0xFFu);
  const std::uint32_t size = payload[2] & 0xFFFu;
  if (std::uint64_t(address) + std::uint64_t(size) * 4u >
      memory::kPhysicalMemorySize) {
    throw std::out_of_range("PM4_LOAD_ALU_CONSTANT outside physical RAM");
  }

  std::vector<std::byte> snapshot(std::size_t(size) * 4u);
  if (!memory_.copy_physical_range(address, snapshot)) {
    throw std::runtime_error("PM4_LOAD_ALU_CONSTANT snapshot failed");
  }
  for (std::uint32_t i = 0; i < size; ++i) {
    emit_register_write(
        index++, load_be32(snapshot.data() + std::size_t(i) * 4u));
  }
  ++stats_.memory_constant_load_packets;
}

void CommandProcessor::execute_set_shader_bases(
    std::span<const std::uint32_t> payload) {
  if (payload.size() != 1u) {
    throw std::runtime_error("PM4_SET_SHADER_BASES requires exactly 1 dword");
  }
  const std::uint32_t raw = payload[0];
  shader_partition_.raw = raw;
  shader_partition_.instruction_store_size_code = (raw >> 29u) & 0x7u;
  shader_partition_.vertex_start = (raw >> 16u) & 0xFFFu;
  shader_partition_.pixel_start = raw & 0xFFFu;
  ++stats_.shader_base_packets;
}

bool CommandProcessor::execute_im_store(
    std::span<const std::uint32_t> payload) {
  if (payload.size() != 2u) {
    throw std::runtime_error("PM4_IM_STORE requires exactly 2 dwords");
  }
  const std::uint32_t stage_bits = payload[0] & 0x3u;
  // Public Xenos implementations expose vertex/pixel instruction stores. The
  // A2xx-family shared-store selector (2) isn't promoted to Xenos semantics
  // without title/hardware evidence, so retain such a packet losslessly.
  if (stage_bits > 1u) return false;

  const auto& program = stage_bits == 0u ? active_vertex_program_
                                          : active_pixel_program_;
  if (!program) {
    throw std::runtime_error("PM4_IM_STORE has no active shader program");
  }
  if (program->start_slot() > 0xFFFFu || program->dwords().size() > 0xFFFFu) {
    throw std::runtime_error("PM4_IM_STORE shader metadata overflow");
  }

  const std::uint32_t destination = cpu_to_gpu_address(payload[0] & ~0x3u);
  const std::uint64_t byte_count64 =
      std::uint64_t(program->dwords().size()) * 4u;
  if (byte_count64 > UINT32_MAX ||
      std::uint64_t(destination) + byte_count64 > memory::kPhysicalMemorySize) {
    throw std::out_of_range("PM4_IM_STORE destination outside physical RAM");
  }

  if (byte_count64) {
    auto write = memory_.physical_write_span(
        destination, static_cast<std::uint32_t>(byte_count64));
    if (!write) throw std::runtime_error("PM4_IM_STORE has no destination backing");
    std::uint32_t offset = 0;
    for (const std::uint32_t dword : program->dwords()) {
      const auto encoded = encode_be32(dword);
      if (!write.write(offset, encoded)) {
        throw std::runtime_error("PM4_IM_STORE write span overflow");
      }
      offset += 4u;
    }
  }

  // IM_STORE also saves the instruction-store start/size descriptor into the
  // command/state shadow supplied by the second dword. This lets a later
  // IM_LOAD restore exactly the bank that was saved.
  const std::uint32_t metadata_address = cpu_to_gpu_address(payload[1]);
  const std::uint32_t start_size =
      (program->start_slot() << 16u) |
      static_cast<std::uint32_t>(program->dwords().size());
  const auto encoded_metadata = encode_be32(start_size);
  if (!memory_.write_physical(metadata_address, encoded_metadata)) {
    throw std::runtime_error("PM4_IM_STORE metadata write failed");
  }

  ++stats_.shader_store_packets;
  stats_.shader_store_dwords += program->dwords().size();
  stats_.physical_writes += program->dwords().size() + 1u;
  return true;
}

void CommandProcessor::execute_invalidate_state(
    std::span<const std::uint32_t> payload) {
  if (payload.size() != 1u) {
    throw std::runtime_error("PM4_INVALIDATE_STATE requires exactly 1 dword");
  }
  const std::uint32_t mask = payload[0];
  // A2xx/Xenos-era context streams identify 0x100 and 0x200 specifically as
  // vertex and pixel instruction-code address/size invalidation. Other bits
  // remain preserved in IR until their Xenos meaning is independently known.
  if ((mask & 0x100u) != 0u) {
    active_vertex_shader_ = {};
    active_vertex_program_.reset();
    ++stats_.shader_invalidations;
  }
  if ((mask & 0x200u) != 0u) {
    active_pixel_shader_ = {};
    active_pixel_program_.reset();
    ++stats_.shader_invalidations;
  }
  ++stats_.invalidate_state_packets;
}

void CommandProcessor::execute_event_write(
    std::span<const std::uint32_t> payload) {
  if (payload.empty()) {
    throw std::runtime_error("PM4_EVENT_WRITE missing initiator");
  }
  // The one-dword form is the well-established event-initiator write. Longer
  // forms are retained in EventPacket IR, but their additional destination
  // semantics are not assigned without Xenos-specific evidence.
  emit_register_write(kVgtEventInitiator, payload[0] & 0x3Fu);
  ++stats_.event_packets;
}

void CommandProcessor::execute_event_write_shader_done(
    std::span<const std::uint32_t> payload) {
  if (payload.size() != 3u) {
    throw std::runtime_error(
        "PM4_EVENT_WRITE_SHD requires initiator, address and value");
  }

  const std::uint32_t initiator = payload[0];
  emit_register_write(kVgtEventInitiator, initiator & 0x3Fu);
  const bool write_progress_counter = (initiator & 0x80000000u) != 0u;
  const std::uint32_t value = write_progress_counter ? swap_counter_ : payload[2];
  if ((payload[1] & ~0x3u) != 0u) {
    write_physical_dword(payload[1], value);
    ++stats_.event_memory_writes;
    if (write_progress_counter) ++stats_.event_counter_writes;
  }
  ++stats_.event_packets;
}

void CommandProcessor::execute_event_write_extent(
    std::span<const std::uint32_t> payload) {
  if (payload.size() != 2u) {
    throw std::runtime_error(
        "PM4_EVENT_WRITE_EXT requires initiator and address");
  }

  emit_register_write(kVgtEventInitiator, payload[0] & 0x3Fu);
  const Endian endian = static_cast<Endian>(payload[1] & 0x3u);
  if (endian != Endian::Swap8In16) {
    throw std::runtime_error(
        "PM4_EVENT_WRITE_EXT requires Xenos 8-in-16 destination endian");
  }
  const std::uint32_t address = cpu_to_gpu_address(payload[1] & ~0x3u);
  if (std::uint64_t(address) + 12u > memory::kPhysicalMemorySize) {
    throw std::out_of_range("PM4_EVENT_WRITE_EXT destination outside RAM");
  }

  // Until the backend exposes a real raster extent query, report the complete
  // architectural 2D range. This is intentionally conservative: it may
  // over-report affected pixels, but never hides work from the guest runtime.
  constexpr std::array<std::uint16_t, 6> extents = {
      0u, static_cast<std::uint16_t>(kTexture2DCubeMaxWidthHeight >> 3u),
      0u, static_cast<std::uint16_t>(kTexture2DCubeMaxWidthHeight >> 3u),
      0u, 1u};
  std::array<std::byte, 12> encoded{};
  for (std::size_t i = 0; i < extents.size(); ++i) {
    const auto value = encode_be16(extents[i]);
    encoded[i * 2u] = value[0];
    encoded[i * 2u + 1u] = value[1];
  }
  if (!memory_.write_physical(address, encoded)) {
    throw std::runtime_error("PM4_EVENT_WRITE_EXT write failed");
  }
  stats_.physical_writes += 3u;
  ++stats_.event_memory_writes;
  ++stats_.extent_event_writes;
  ++stats_.event_packets;
}

void CommandProcessor::execute_viz_query(
    std::span<const std::uint32_t> payload) {
  if (payload.size() != 1u) {
    throw std::runtime_error("PM4_VIZ_QUERY requires exactly 1 dword");
  }
  const std::uint32_t id = payload[0] & 0x3Fu;
  const bool end = (payload[0] & 0x100u) != 0u;
  const std::uint64_t bit = UINT64_C(1) << id;
  const std::uint32_t status_register =
      id < 32u ? kPaScVizQueryStatus0 : kPaScVizQueryStatus1;
  const std::uint32_t status_bit = UINT32_C(1) << (id & 31u);

  if (!end) {
    active_viz_queries_ |= bit;
    viz_query_draws_ &= ~bit;
    emit_register_write(status_register,
                        registers_.read(status_register) & ~status_bit);
    emit_register_write(kVgtEventInitiator, 7u);  // VIZQUERY_START.
    ++stats_.viz_query_begins;
  } else {
    active_viz_queries_ &= ~bit;
    emit_register_write(kVgtEventInitiator, 8u);  // VIZQUERY_END.
    if ((viz_query_draws_ & bit) != 0u) {
      emit_register_write(status_register,
                          registers_.read(status_register) | status_bit);
      ++stats_.viz_query_visible_results;
    }
    viz_query_draws_ &= ~bit;
    ++stats_.viz_query_ends;
  }
  ++stats_.event_packets;
}

void CommandProcessor::execute_interrupt(
    std::span<const std::uint32_t> payload) {
  if (payload.size() != 1u) {
    throw std::runtime_error("PM4_INTERRUPT requires exactly 1 dword");
  }
  ++stats_.interrupt_packets;
  if (!interrupt_callback_) return;
  for (std::uint32_t cpu = 0; cpu < 6u; ++cpu) {
    if ((payload[0] & (UINT32_C(1) << cpu)) == 0u) continue;
    interrupt_callback_(cpu);
    ++stats_.interrupt_dispatches;
  }
}

void CommandProcessor::execute_mem_write(std::span<const std::uint32_t> payload) {
  if (payload.empty()) throw std::runtime_error("PM4_MEM_WRITE missing address");
  if (payload.size() == 1u) return;

  const auto address_with_endian = payload[0];
  const Endian endian = static_cast<Endian>(address_with_endian & 0x3u);
  const auto physical_base =
      cpu_to_gpu_address(address_with_endian & ~0x3u);
  const auto word_count = payload.size() - 1u;
  const auto byte_count64 = std::uint64_t{word_count} * 4u;
  if (byte_count64 > std::numeric_limits<std::uint32_t>::max() ||
      std::uint64_t{physical_base} + byte_count64 > memory::kPhysicalMemorySize) {
    throw std::out_of_range("Xenos physical write outside RAM");
  }

  auto write = memory_.physical_write_span(
      physical_base, static_cast<std::uint32_t>(byte_count64));
  if (!write) throw std::runtime_error("Xenos physical write has no backing");
  for (std::size_t i = 0; i < word_count; ++i) {
    const auto logical_value = payload[i + 1u];
    const auto stored = gpu_swap(logical_value, endian);
    const auto encoded = encode_le32(stored);
    if (!write.write(static_cast<std::uint32_t>(i * 4u), encoded)) {
      throw std::runtime_error("Xenos physical write span overflow");
    }
    stream_.emit(ir::PhysicalMemoryWrite{
        static_cast<std::uint32_t>(physical_base + i * 4u), logical_value, endian});
  }
  stats_.physical_writes += word_count;
}

void CommandProcessor::execute_draw(Type3Opcode opcode, bool predicate,
                                    std::vector<std::uint32_t> payload) {
  const bool has_viz_token =
      opcode == Type3Opcode::DrawIndx || opcode == Type3Opcode::DrawIndxBin;
  const bool binned =
      opcode == Type3Opcode::DrawIndxBin || opcode == Type3Opcode::DrawIndx2Bin;

  const std::size_t initiator_index = has_viz_token ? 1u : 0u;
  if (payload.size() <= initiator_index) {
    throw std::runtime_error("Xenos draw packet missing VGT_DRAW_INITIATOR");
  }

  const std::uint32_t initiator_value = payload[initiator_index];
  const auto initiator = decode_draw_initiator(initiator_value);

  // DRAW packets program these registers as part of packet execution. Emit the
  // implicit register write so backend shadow state remains hardware-faithful.
  emit_register_write(kVgtDrawInitiator, initiator_value);

  ir::DrawPacket draw{};
  draw.opcode = opcode;
  draw.predicate = predicate;
  draw.primitive_type = initiator.primitive;
  draw.source = initiator.source;
  draw.major_mode = initiator.major_mode;
  draw.explicit_major_mode =
      is_explicit_major_mode(initiator.major_mode, initiator.primitive);
  draw.index_format = initiator.index_format;
  draw.not_eop = initiator.not_eop;
  draw.binned = binned;
  draw.index_count = initiator.index_count;
  draw.viz_query_condition = has_viz_token ? payload[0] : 0u;
  draw.vertex_shader = active_vertex_shader_;
  draw.pixel_shader = active_pixel_shader_;

  std::size_t tail_index = initiator_index + 1u;

  // The binned variants insert BIN_BASE and BIN_SIZE immediately after the
  // draw initiator, before the ordinary source-specific index data. This is
  // the same packet layout documented by the AMD/Yamato A2xx command stream,
  // whose PM4 packet family shares these Xenos-era opcodes. The original GPU
  // uses the data for visibility/bin predication; a native full-target renderer
  // executes the normalized draw once and does not replay that tiling pass.
  if (binned) {
    if (payload.size() < tail_index + 2u) {
      throw std::runtime_error(
          "Xenos binned draw missing BIN_BASE/BIN_SIZE");
    }
    draw.binning.valid = true;
    draw.binning.base = payload[tail_index++];
    draw.binning.size = payload[tail_index++];
    draw.binning.base_offset = bin_base_offset_;
    draw.binning.effective_base =
        draw.binning.base + draw.binning.base_offset;
    draw.binning.mask = bin_mask_;
    draw.binning.select = bin_select_;
  }

  switch (initiator.source) {
    case DrawSource::Dma: {
      if (payload.size() < tail_index + 2u) {
        throw std::runtime_error(
            "Xenos DMA draw missing VGT_DMA_BASE/VGT_DMA_SIZE");
      }
      const std::uint32_t raw_base = payload[tail_index++];
      const std::uint32_t dma_size = payload[tail_index++];
      emit_register_write(kVgtDmaBase, raw_base);
      emit_register_write(kVgtDmaSize, dma_size);

      const std::uint32_t element_bytes = index_size_bytes(initiator.index_format);
      const std::uint32_t address =
          cpu_to_gpu_address(raw_base) & ~(element_bytes - 1u);
      const std::uint32_t num_words = dma_size & 0x00FFFFFFu;
      const std::uint64_t length64 =
          std::uint64_t(num_words) * std::uint64_t(element_bytes);
      if (length64 > UINT32_MAX) {
        throw std::out_of_range("Xenos index buffer length overflow");
      }
      const auto length = static_cast<std::uint32_t>(length64);
      if (std::uint64_t(address) + length > memory::kPhysicalMemorySize) {
        throw std::out_of_range("Xenos index buffer outside physical RAM");
      }

      draw.index_buffer.valid = true;
      draw.index_buffer.physical_address = address;
      draw.index_buffer.length_bytes = length;
      draw.index_buffer.index_count = initiator.index_count;
      draw.index_buffer.format = initiator.index_format;
      draw.index_buffer.endian =
          static_cast<Endian>((dma_size >> 30) & 0x3u);
      break;
    }

    case DrawSource::Immediate: {
      // Preserve immediate indices in their packet packing. The future
      // primitive conversion stage will unpack/convert them once, before any
      // Vulkan/D3D12 backend sees the draw. For binned packets, BIN_BASE and
      // BIN_SIZE have already been consumed above, so the remainder is the same
      // source-specific immediate payload as the non-binned form.
      if (tail_index < payload.size()) {
        draw.immediate_index_dwords.assign(
            payload.begin() + static_cast<std::ptrdiff_t>(tail_index),
            payload.end());
      }
      break;
    }

    case DrawSource::AutoIndex:
    case DrawSource::Reserved:
      break;
  }

  // Until backend sample-count feedback is wired into the common frontend,
  // treat any successfully decoded draw inside a visibility-query scope as
  // visible. Empty query scopes still report not-visible, which is less
  // destructive than unconditionally marking every query visible.
  viz_query_draws_ |= active_viz_queries_;

  draw.register_generation = registers_.generation();
  draw.raw_payload = std::move(payload);
  stream_.emit(std::move(draw));
  ++stats_.draws;
}

void CommandProcessor::execute_type3(Reader& reader, const PacketHeader& header,
                                     std::uint32_t depth) {
  auto payload = read_payload(reader, header.count);

  // Type-3 predication is command-processor state, not draw/backend state.
  // A failed predicate suppresses the complete packet after its payload has
  // been consumed from the ring, before it can mutate state or emit IR.
  if (header.predicate && !predicate_passes()) {
    ++stats_.predicated_packets_skipped;
    return;
  }

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

  if (header.opcode == Type3Opcode::RegRmw) {
    execute_reg_rmw(payload);
    stream_.emit(ir::StatePacket{header.opcode, payload});
    return;
  }

  if (header.opcode == Type3Opcode::RegToMem) {
    execute_reg_to_mem(payload);
    return;
  }

  if (header.opcode == Type3Opcode::CondExec) {
    execute_cond_exec(reader, payload);
    stream_.emit(ir::StatePacket{header.opcode, payload});
    return;
  }

  if (header.opcode == Type3Opcode::CondWrite) {
    execute_cond_write(payload);
    stream_.emit(ir::StatePacket{header.opcode, payload});
    return;
  }

  if (header.opcode == Type3Opcode::WaitRegMem) {
    execute_wait_reg_mem(payload);
    stream_.emit(ir::SynchronizationPacket{header.opcode, payload});
    return;
  }

  if (header.opcode == Type3Opcode::WaitRegEq ||
      header.opcode == Type3Opcode::WaitRegGte) {
    execute_wait_register(payload, header.opcode == Type3Opcode::WaitRegGte);
    stream_.emit(ir::SynchronizationPacket{header.opcode, payload});
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

  if (header.opcode == Type3Opcode::SetShaderBases) {
    execute_set_shader_bases(payload);
    stream_.emit(ir::ShaderPacket{header.opcode, payload});
    return;
  }

  if (header.opcode == Type3Opcode::InvalidateState) {
    execute_invalidate_state(payload);
    stream_.emit(ir::StatePacket{header.opcode, payload});
    return;
  }

  if (header.opcode == Type3Opcode::ImStore) {
    if (execute_im_store(payload)) {
      stream_.emit(ir::ShaderPacket{header.opcode, payload});
      return;
    }
    // Unknown/shared instruction-store selectors remain losslessly represented.
    stream_.emit(ir::ShaderPacket{header.opcode, std::move(payload)});
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
    std::vector<std::byte> microcode_snapshot(std::size_t(size) * 4u);
    if (!memory_.copy_physical_range(address, microcode_snapshot)) {
      throw std::runtime_error("PM4_IM_LOAD shader snapshot failed");
    }
    std::vector<std::uint32_t> microcode;
    microcode.reserve(size);
    for (std::uint32_t i = 0; i < size; ++i) {
      microcode.push_back(
          load_be32(microcode_snapshot.data() + std::size_t(i) * 4u));
    }
    ShaderProgram program(stage, microcode, start);
    const ir::ShaderReference reference{true, program.hash(), start};
    if (stage == ShaderStage::Vertex) {
      active_vertex_shader_ = reference;
      active_vertex_program_ = program;
    } else {
      active_pixel_shader_ = reference;
      active_pixel_program_ = program;
    }
    auto decoded = ShaderDecoder::decode(program);
    stream_.emit(ir::ShaderLoad{header.opcode, false, address,
                                std::move(program), std::move(decoded), payload});
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
    ShaderProgram program(stage, microcode, start);
    const ir::ShaderReference reference{true, program.hash(), start};
    if (stage == ShaderStage::Vertex) {
      active_vertex_shader_ = reference;
      active_vertex_program_ = program;
    } else {
      active_pixel_shader_ = reference;
      active_pixel_program_ = program;
    }
    auto decoded = ShaderDecoder::decode(program);
    stream_.emit(ir::ShaderLoad{header.opcode, true, 0,
                                std::move(program), std::move(decoded), payload});
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
    std::vector<std::byte> constant_snapshot(std::size_t(size) * 4u);
    if (!memory_.copy_physical_range(address, constant_snapshot)) {
      throw std::runtime_error("PM4_LOAD_CONSTANT_CONTEXT snapshot failed");
    }
    for (std::uint32_t i = 0; i < size; ++i) {
      emit_register_write(
          index++,
          load_be32(constant_snapshot.data() + std::size_t(i) * 4u));
    }
    stream_.emit(ir::StatePacket{header.opcode, payload});
    return;
  }

  if (header.opcode == Type3Opcode::LoadAluConstant) {
    execute_load_alu_constant(payload);
    stream_.emit(ir::StatePacket{header.opcode, payload});
    return;
  }

  if (header.opcode == Type3Opcode::VizQuery) {
    execute_viz_query(payload);
    stream_.emit(ir::EventPacket{header.opcode, payload});
    return;
  }

  if (header.opcode == Type3Opcode::EventWrite) {
    execute_event_write(payload);
    stream_.emit(ir::EventPacket{header.opcode, payload});
    return;
  }

  if (header.opcode == Type3Opcode::EventWriteShaderDone) {
    execute_event_write_shader_done(payload);
    stream_.emit(ir::EventPacket{header.opcode, payload});
    return;
  }

  if (header.opcode == Type3Opcode::EventWriteExtent) {
    execute_event_write_extent(payload);
    stream_.emit(ir::EventPacket{header.opcode, payload});
    return;
  }

  if (header.opcode == Type3Opcode::Interrupt) {
    execute_interrupt(payload);
    stream_.emit(ir::EventPacket{header.opcode, payload});
    return;
  }

  if (header.opcode == Type3Opcode::SetBinBaseOffset) {
    if (payload.empty()) {
      throw std::runtime_error("PM4_SET_BIN_BASE_OFFSET missing value");
    }
    bin_base_offset_ = payload[0];
    stream_.emit(ir::StatePacket{header.opcode, payload});
    return;
  }

  if (header.opcode == Type3Opcode::SetBinMask ||
      header.opcode == Type3Opcode::SetBinSelect) {
    if (payload.empty()) {
      throw std::runtime_error("64-bit Xenos bin state packet missing value");
    }
    std::uint64_t value = payload[0];
    if (payload.size() > 1u) value |= std::uint64_t(payload[1]) << 32u;
    if (header.opcode == Type3Opcode::SetBinMask) {
      bin_mask_ = value;
    } else {
      bin_select_ = value;
    }
    stream_.emit(ir::StatePacket{header.opcode, payload});
    return;
  }

  if (header.opcode == Type3Opcode::SetBinMaskLow ||
      header.opcode == Type3Opcode::SetBinMaskHigh ||
      header.opcode == Type3Opcode::SetBinSelectLow ||
      header.opcode == Type3Opcode::SetBinSelectHigh) {
    if (payload.empty()) {
      throw std::runtime_error("Xenos bin state half-write missing value");
    }
    const std::uint64_t low_mask = UINT64_C(0x00000000FFFFFFFF);
    const std::uint64_t high_mask = UINT64_C(0xFFFFFFFF00000000);
    switch (header.opcode) {
      case Type3Opcode::SetBinMaskLow:
        bin_mask_ = (bin_mask_ & high_mask) | payload[0];
        break;
      case Type3Opcode::SetBinMaskHigh:
        bin_mask_ = (bin_mask_ & low_mask) | (std::uint64_t(payload[0]) << 32u);
        break;
      case Type3Opcode::SetBinSelectLow:
        bin_select_ = (bin_select_ & high_mask) | payload[0];
        break;
      case Type3Opcode::SetBinSelectHigh:
        bin_select_ = (bin_select_ & low_mask) |
                      (std::uint64_t(payload[0]) << 32u);
        break;
      default:
        break;
    }
    stream_.emit(ir::StatePacket{header.opcode, payload});
    return;
  }

  if (is_draw_opcode(header.opcode)) {
    execute_draw(header.opcode, header.predicate, std::move(payload));
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

bool CommandProcessor::predicate_passes() const noexcept {
  return (bin_select_ & bin_mask_) != 0;
}

}  // namespace xenon::gpu
