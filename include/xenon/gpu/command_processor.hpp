#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "xenon/gpu/ir.hpp"
#include "xenon/gpu/register_file.hpp"
#include "xenon/memory/address_space.hpp"

namespace xenon::gpu {

class CommandProcessor {
 public:
  struct Statistics {
    std::uint64_t packets{};
    std::uint64_t type0_packets{};
    std::uint64_t type1_packets{};
    std::uint64_t type2_packets{};
    std::uint64_t type3_packets{};
    std::uint64_t indirect_buffers{};
    std::uint64_t draws{};
    std::uint64_t physical_writes{};
  };

  CommandProcessor(memory::AddressSpace& memory, RegisterFile& registers,
                   ir::Stream& stream);

  // Execute a contiguous PM4 buffer from physical RAM. The buffer words are
  // stored in Xbox/CPU big-endian byte order and are decoded into host logical
  // dwords before packet processing.
  void execute_buffer(std::uint32_t physical_address,
                      std::uint32_t dword_count);

  // Execute the unread portion of a circular primary ring. Indices are in
  // dwords, matching the hardware read/write pointers. Returns the new read
  // index (normally write_index).
  [[nodiscard]] std::uint32_t execute_ring(std::uint32_t physical_address,
                                           std::uint32_t capacity_dwords,
                                           std::uint32_t read_index,
                                           std::uint32_t write_index);

  void reset();

  [[nodiscard]] const Statistics& statistics() const noexcept { return stats_; }
  [[nodiscard]] std::uint32_t max_indirect_depth() const noexcept {
    return max_indirect_depth_;
  }

 private:
  static constexpr std::uint32_t kMaximumIndirectDepth = 32;
  static constexpr std::uint64_t kMaximumDwordsPerSubmission = 16ull * 1024ull * 1024ull;

  class Reader;

  void execute_buffer_internal(std::uint32_t physical_address,
                               std::uint32_t dword_count,
                               std::uint32_t depth);
  void execute_packet(Reader& reader, std::uint32_t depth);
  void execute_type0(Reader& reader, const PacketHeader& header);
  void execute_type1(Reader& reader, const PacketHeader& header);
  void execute_type3(Reader& reader, const PacketHeader& header,
                     std::uint32_t depth);

  [[nodiscard]] std::vector<std::uint32_t> read_payload(
      Reader& reader, std::uint32_t count);
  void emit_register_write(std::uint32_t index, std::uint32_t value);
  void execute_mem_write(std::span<const std::uint32_t> payload);
  void execute_draw(Type3Opcode opcode, bool predicate,
                    std::vector<std::uint32_t> payload);
  void write_physical_dword(std::uint32_t address_with_endian,
                            std::uint32_t logical_value);

  memory::AddressSpace& memory_;
  RegisterFile& registers_;
  ir::Stream& stream_;
  Statistics stats_{};
  ir::ShaderReference active_vertex_shader_{};
  ir::ShaderReference active_pixel_shader_{};
  std::uint32_t bin_base_offset_{};
  std::uint64_t bin_mask_{};
  std::uint64_t bin_select_{};
  std::uint64_t submission_dwords_{};
  std::uint32_t max_indirect_depth_{};
};

}  // namespace xenon::gpu
