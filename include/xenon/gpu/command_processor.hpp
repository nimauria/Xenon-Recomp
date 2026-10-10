#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "xenon/gpu/ir.hpp"
#include "xenon/gpu/register_file.hpp"
#include "xenon/memory/address_space.hpp"

namespace xenon::gpu {

class CommandProcessor {
 public:
  struct ShaderPartitionState {
    std::uint32_t raw{};
    std::uint32_t instruction_store_size_code{};
    std::uint32_t vertex_start{};
    std::uint32_t pixel_start{};
  };

  struct Statistics {
    std::uint64_t packets{};
    std::uint64_t type0_packets{};
    std::uint64_t type1_packets{};
    std::uint64_t type2_packets{};
    std::uint64_t type3_packets{};
    std::uint64_t indirect_buffers{};
    std::uint64_t draws{};
    std::uint64_t physical_writes{};
    std::uint64_t predicated_packets_skipped{};
    std::uint64_t register_rmw_packets{};
    std::uint64_t register_to_memory_packets{};
    std::uint64_t register_to_memory_dwords{};
    std::uint64_t conditional_write_packets{};
    std::uint64_t conditional_writes_taken{};
    std::uint64_t conditional_exec_packets{};
    std::uint64_t conditional_exec_taken{};
    std::uint64_t conditional_exec_dwords_skipped{};
    std::uint64_t wait_reg_mem_packets{};
    std::uint64_t wait_reg_mem_polls{};
    std::uint64_t wait_register_packets{};
    std::uint64_t wait_register_polls{};
    // Ring-level waits that could not be satisfied yet and were suspended (the ring
    // read index stays at the wait packet and execution resumes there later).
    std::uint64_t wait_stalls{};
    // COHER_STATUS_HOST requests completed by a WAIT_REG_MEM poll.
    std::uint64_t coherency_syncs{};
    // SCRATCH_REGn writes mirrored to memory at SCRATCH_ADDR + n*4.
    std::uint64_t scratch_writebacks{};
    std::uint64_t memory_constant_load_packets{};
    std::uint64_t shader_base_packets{};
    std::uint64_t shader_store_packets{};
    std::uint64_t shader_store_dwords{};
    std::uint64_t invalidate_state_packets{};
    std::uint64_t shader_invalidations{};
    std::uint64_t event_packets{};
    std::uint64_t event_memory_writes{};
    std::uint64_t event_counter_writes{};
    std::uint64_t extent_event_writes{};
    std::uint64_t viz_query_begins{};
    std::uint64_t viz_query_ends{};
    std::uint64_t viz_query_visible_results{};
    std::uint64_t interrupt_packets{};
    std::uint64_t interrupt_dispatches{};
    // Part 7 of the AC6 Runtime Readiness pass ("GPU capability / silent
    // fallback audit"): a guest register write whose index is outside the
    // known register file. emit_register_write() still throws for this (a
    // deliberate, pre-existing hard-fail for corrupt/invalid command
    // streams - not something this pass changes), but the count is
    // incremented immediately beforehand so it is observable in a
    // capability report even when a caller only sees the resulting
    // exception, not this object.
    std::uint64_t unknown_register_writes{};
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
  // Guest presentation / swap progress. EVENT_WRITE_SHD may request this
  // counter rather than its literal payload value (initiator bit 31).
  void notify_present() noexcept { ++swap_counter_; }
  [[nodiscard]] std::uint32_t swap_counter() const noexcept {
    return swap_counter_;
  }
  void set_interrupt_callback(
      std::function<void(std::uint32_t cpu_index)> callback) {
    interrupt_callback_ = std::move(callback);
  }

  // The WAIT_REG_MEM the most recent stalled drain is parked on (diagnostics: a
  // title stuck at this point is waiting on state nothing in the host model sets).
  struct WaitStallInfo {
    bool valid{false};
    bool memory{false};          // polling memory (else a register)
    std::uint32_t address{};     // physical address (with endian bits) or register index
    std::uint32_t wait_info{};
    std::uint32_t reference{};
    std::uint32_t mask{};
    std::uint32_t last_value{};
  };
  [[nodiscard]] const WaitStallInfo& last_wait_stall() const noexcept { return last_wait_stall_; }

  [[nodiscard]] const Statistics& statistics() const noexcept { return stats_; }
  [[nodiscard]] std::uint32_t max_indirect_depth() const noexcept {
    return max_indirect_depth_;
  }
  [[nodiscard]] const ShaderPartitionState& shader_partition() const noexcept {
    return shader_partition_;
  }
  [[nodiscard]] const std::optional<ShaderProgram>& active_vertex_program() const noexcept {
    return active_vertex_program_;
  }
  [[nodiscard]] const std::optional<ShaderProgram>& active_pixel_program() const noexcept {
    return active_pixel_program_;
  }

 private:
  static constexpr std::uint32_t kMaximumIndirectDepth = 32;
  static constexpr std::uint32_t kMaximumWaitPollIterations = 1u << 20;
  // COHER_STATUS_HOST (xenia register_table.inc 0x0A31): bit 31 is the request-
  // pending flag the hardware sets when the register is written.
  static constexpr std::uint32_t kCoherStatusHostRegister = 0x0A31u;
  static constexpr std::uint32_t kCoherStatusPending = 0x80000000u;
  // Scratch write-through registers (xenia register_table.inc).
  static constexpr std::uint32_t kScratchUmskRegister = 0x01DCu;
  static constexpr std::uint32_t kScratchAddrRegister = 0x01DDu;
  static constexpr std::uint32_t kScratchRegisterFirst = 0x0578u;
  static constexpr std::uint32_t kScratchRegisterLast = 0x057Fu;
  // A wait at ring level that is still false after this many polls is suspended
  // instead of spun on: the hardware command processor never blocks the host
  // thread that also has to deliver vsync interrupts, and the value it waits for
  // is usually written by CPU code (a callback, another thread) that needs those
  // interrupts to run.
  static constexpr std::uint32_t kStallableWaitPollIterations = 256;
  // Control-flow signal from a WAIT packet to execute_ring(); never escapes it.
  struct WaitStalled final {};
  static constexpr std::uint64_t kMaximumDwordsPerSubmission = 16ull * 1024ull * 1024ull;

  class Reader;

  void execute_buffer_internal(std::uint32_t physical_address,
                               std::uint32_t dword_count,
                               std::uint32_t depth,
                               std::uint32_t start_offset = 0);
  void execute_packet(Reader& reader, std::uint32_t depth);
  void execute_type0(Reader& reader, const PacketHeader& header);
  void execute_type1(Reader& reader, const PacketHeader& header);
  void execute_type3(Reader& reader, const PacketHeader& header,
                     std::uint32_t depth);

  [[nodiscard]] std::vector<std::uint32_t> read_payload(
      Reader& reader, std::uint32_t count);
  void emit_register_write(std::uint32_t index, std::uint32_t value);
  void execute_mem_write(std::span<const std::uint32_t> payload);
  void execute_reg_rmw(std::span<const std::uint32_t> payload);
  void execute_reg_to_mem(std::span<const std::uint32_t> payload);
  void execute_cond_exec(Reader& reader,
                         std::span<const std::uint32_t> payload);
  void execute_cond_write(std::span<const std::uint32_t> payload);
  void execute_wait_reg_mem(std::span<const std::uint32_t> payload);
  void execute_wait_register(std::span<const std::uint32_t> payload,
                             bool greater_or_equal);
  void execute_load_alu_constant(std::span<const std::uint32_t> payload);
  void execute_set_shader_bases(std::span<const std::uint32_t> payload);
  bool execute_im_store(std::span<const std::uint32_t> payload);
  void execute_invalidate_state(std::span<const std::uint32_t> payload);
  void execute_event_write(std::span<const std::uint32_t> payload);
  void execute_event_write_shader_done(
      std::span<const std::uint32_t> payload);
  void execute_event_write_extent(std::span<const std::uint32_t> payload);
  void execute_viz_query(std::span<const std::uint32_t> payload);
  void execute_interrupt(std::span<const std::uint32_t> payload);
  void execute_draw(Type3Opcode opcode, bool predicate,
                    std::vector<std::uint32_t> payload);
  void write_physical_dword(std::uint32_t address_with_endian,
                            std::uint32_t logical_value);
  [[nodiscard]] std::uint32_t read_physical_dword(
      std::uint32_t address_with_endian) const;
  [[nodiscard]] static bool wait_condition_matches(
      std::uint32_t wait_info, std::uint32_t value, std::uint32_t reference,
      std::uint32_t mask) noexcept;
  [[nodiscard]] bool predicate_passes() const noexcept;

  memory::AddressSpace& memory_;
  RegisterFile& registers_;
  ir::Stream& stream_;
  Statistics stats_{};
  ir::ShaderReference active_vertex_shader_{};
  ir::ShaderReference active_pixel_shader_{};
  std::optional<ShaderProgram> active_vertex_program_{};
  std::optional<ShaderProgram> active_pixel_program_{};
  ShaderPartitionState shader_partition_{};
  std::uint32_t bin_base_offset_{};
  std::uint64_t bin_mask_{};
  std::uint64_t bin_select_{};
  std::uint64_t active_viz_queries_{};
  std::uint64_t viz_query_draws_{};
  std::uint32_t swap_counter_{};
  std::function<void(std::uint32_t cpu_index)> interrupt_callback_{};
  std::uint64_t submission_dwords_{};
  std::uint32_t max_indirect_depth_{};
  // True while execute_ring() runs: an unsatisfied wait then suspends the drain
  // (see kStallableWaitPollIterations) instead of spinning. execute_buffer()
  // callers (captures, tools) keep the blocking behaviour.
  bool stall_on_unsatisfied_wait_{false};
  WaitStallInfo last_wait_stall_{};

  // Where a suspended wait sits inside nested indirect buffers, so the next drain
  // resumes AT the wait rather than replaying the buffer from its start.
  struct ResumeFrame {
    std::uint32_t address{};
    std::uint32_t length{};
    std::uint32_t offset{};  // dword offset of the packet being executed
  };
  std::vector<ResumeFrame> stall_frames_{};   // collected innermost-first while unwinding
  std::vector<ResumeFrame> resume_frames_{};  // outermost-first, applied on the next drain
  std::uint32_t resume_ring_index_{};         // ring packet the suspended wait lives in
  std::size_t resume_cursor_{};
  bool resume_pending_{false};
  bool resuming_{false};
};

}  // namespace xenon::gpu
