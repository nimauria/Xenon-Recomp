#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>

#include "xenon/cpu/types.hpp"
#include "xenon/memory/address_space.hpp"

namespace xenon::audio {

inline constexpr std::size_t kXmaContextCount = 320;
inline constexpr std::size_t kXmaContextBytes = 64;
inline constexpr std::size_t kXmaPacketBytes = 2048;
inline constexpr std::uint32_t kXmaSamplesPerFrame = 512;
inline constexpr std::uint32_t kXmaSamplesPerSubframe = 128;
inline constexpr std::uint32_t kXmaOutputBlockBytes = 256;
inline constexpr cpu::GuestAddress kXmaMmioBase = 0x7FEA0000u;
inline constexpr std::uint32_t kXmaMmioSize = 0x00010000u;

struct XmaPacketHeader {
  std::uint8_t frame_count{};
  std::uint16_t first_frame_offset_bits{};
  std::uint8_t metadata{};
  std::uint8_t skip_count{};
};

[[nodiscard]] XmaPacketHeader parse_xma_packet_header(
    std::span<const std::byte> packet) noexcept;

// Semantic view of the 64-byte hardware context. encode/decode explicitly use
// Xbox big-endian dwords rather than relying on host C++ bitfield layout.
struct XmaContextData {
  std::uint16_t input_buffer_0_packet_count{};
  std::uint8_t loop_count{};
  bool input_buffer_0_valid{};
  bool input_buffer_1_valid{};
  std::uint8_t output_buffer_block_count{};
  std::uint8_t output_buffer_write_offset{};

  std::uint16_t input_buffer_1_packet_count{};
  // Xbox XMA hardware word 1: loop_subframe_end occupies bits 12-13; the
  // following three bits are reserved/unknown; loop_subframe_skip is bits
  // 17-19. subframe_decode_count and output_buffer_padding are measured in
  // 256-byte output blocks (stereo therefore uses two blocks per 128 samples).
  std::uint8_t loop_subframe_end{};
  std::uint8_t reserved_dword_1_a{};
  std::uint8_t loop_subframe_skip{};
  std::uint8_t subframe_decode_count{};
  std::uint8_t output_buffer_padding{};
  std::uint8_t sample_rate{};
  bool is_stereo{};
  bool output_buffer_valid{};

  std::uint32_t input_buffer_read_offset{};
  std::uint8_t error_status{};
  bool error_set{};
  std::uint32_t loop_start{};
  std::uint8_t parser_error_status{};
  bool parser_error_set{};
  std::uint32_t loop_end{};
  std::uint8_t packet_metadata{};
  bool current_buffer{};

  std::uint32_t input_buffer_0_ptr{};
  std::uint32_t input_buffer_1_ptr{};
  std::uint32_t output_buffer_ptr{};
  std::uint32_t work_buffer_ptr{};

  std::uint8_t output_buffer_read_offset{};
  bool stop_when_done{};
  bool interrupt_when_done{};
  std::array<std::uint32_t, 6> reserved{};

  [[nodiscard]] static XmaContextData decode(
      std::span<const std::byte, kXmaContextBytes> bytes) noexcept;
  void encode(std::span<std::byte, kXmaContextBytes> bytes) const noexcept;
};

struct XmaContextInit {
  cpu::GuestAddress input_buffer_0{};
  std::uint32_t input_buffer_0_packet_count{};
  cpu::GuestAddress input_buffer_1{};
  std::uint32_t input_buffer_1_packet_count{};
  std::uint32_t input_buffer_read_offset{};
  cpu::GuestAddress output_buffer{};
  std::uint32_t output_buffer_block_count{};
  cpu::GuestAddress work_buffer{};
  std::uint32_t subframe_decode_count{};
  // Xbox XMA channel-mode selector: 0 = mono, 1 = stereo.
  std::uint32_t channel_count{};
  std::uint32_t sample_rate{};
  std::uint32_t loop_start{};
  std::uint32_t loop_end{};
  std::uint8_t loop_count{};
  std::uint8_t loop_subframe_end{};
  std::uint8_t loop_subframe_skip{};
};

class XmaDecoder final {
 public:
  using CompletionSink =
      std::function<void(cpu::GuestAddress context, bool interrupt_requested)>;
  explicit XmaDecoder(memory::AddressSpace& memory);
  ~XmaDecoder();

  XmaDecoder(const XmaDecoder&) = delete;
  XmaDecoder& operator=(const XmaDecoder&) = delete;

  [[nodiscard]] bool initialize(std::string* error = nullptr);
  void shutdown() noexcept;
  [[nodiscard]] bool initialized() const noexcept { return initialized_.load(); }

  [[nodiscard]] cpu::GuestAddress allocate_context();
  [[nodiscard]] bool release_context(cpu::GuestAddress context);
  [[nodiscard]] bool owns_context(cpu::GuestAddress context) const noexcept;
  [[nodiscard]] std::uint32_t context_array_physical() const noexcept {
    return context_physical_base_;
  }

  [[nodiscard]] bool read_context(cpu::GuestAddress context,
                                  XmaContextData& out) const;
  [[nodiscard]] bool write_context(cpu::GuestAddress context,
                                   const XmaContextData& data,
                                   bool external_write = true);
  [[nodiscard]] bool initialize_context(cpu::GuestAddress context,
                                        const XmaContextInit& init);

  [[nodiscard]] bool enable_context(cpu::GuestAddress context);
  [[nodiscard]] bool disable_context(cpu::GuestAddress context, bool wait);
  [[nodiscard]] bool clear_context(cpu::GuestAddress context);
  [[nodiscard]] bool block_while_in_use(cpu::GuestAddress context);
  [[nodiscard]] std::uint64_t decoded_samples(cpu::GuestAddress context) const;
  void set_completion_sink(CompletionSink sink);

 private:
  struct Runtime;
  [[nodiscard]] std::optional<std::size_t> index_of(
      cpu::GuestAddress context) const noexcept;
  [[nodiscard]] bool resolve_guest_physical(cpu::GuestAddress guest,
                                            std::uint32_t& physical) const;
  [[nodiscard]] bool decode_one(std::size_t index);
  [[nodiscard]] bool merge_hardware_progress(cpu::GuestAddress context,
                                             const XmaContextData& initial,
                                             const XmaContextData& progressed);
  [[nodiscard]] std::uint64_t mmio_read(cpu::GuestAddress address,
                                        std::uint32_t width);
  void mmio_write(cpu::GuestAddress address, std::uint32_t width,
                  std::uint64_t value);
  void notify_completion(cpu::GuestAddress context, bool interrupt_requested);
  void worker_main();

  memory::AddressSpace& memory_;
  std::uint32_t context_physical_base_{};
  cpu::GuestAddress context_guest_base_{};
  std::uint32_t context_allocation_size_{};
  memory::PhysicalPageClass context_page_class_{memory::PhysicalPageClass::Page64K};
  std::array<std::atomic_bool, kXmaContextCount> allocated_{};
  std::array<std::unique_ptr<Runtime>, kXmaContextCount> runtimes_{};
  std::atomic_bool initialized_{false};
  std::atomic_bool worker_running_{false};
  mutable std::mutex worker_mutex_{};
  std::condition_variable worker_cv_{};
  std::thread worker_{};

  // XMA hardware is exposed through the 0x7FEA0000 MMIO aperture as well as
  // xboxkrnl helper exports. Later titles may write the hardware context and
  // kick/lock/clear registers directly, so both paths converge here.
  struct MmioBridge;
  std::shared_ptr<MmioBridge> mmio_bridge_{};
  bool mmio_registered_{};
  mutable std::mutex mmio_mutex_{};
  std::array<std::uint32_t, kXmaMmioSize / 4u> mmio_registers_{};
  std::uint32_t next_context_index_{1};

  mutable std::mutex completion_mutex_{};
  CompletionSink completion_sink_{};
};

}  // namespace xenon::audio
