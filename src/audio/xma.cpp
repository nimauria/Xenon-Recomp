#include "xenon/audio/xma.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#if defined(XENON_AUDIO_FFMPEG_HAS_CH_LAYOUT)
#include <libavutil/channel_layout.h>
#endif
#include <libavutil/samplefmt.h>
}

namespace xenon::audio {
namespace {

constexpr std::uint32_t kBitsPerPacket = kXmaPacketBytes * 8u;
constexpr std::uint32_t kMaxFrameBits = 0x7FFFu;

// XMA register indices from the Xbox 360 APU register file, converted to byte
// offsets inside the 0x7FEA0000 MMIO aperture. Context groups contain ten
// dwords, each controlling 32 of the 320 hardware contexts.
constexpr std::uint32_t kRegContextArrayAddress = 0x0600u * 4u;
constexpr std::uint32_t kRegCurrentContextIndex = 0x0606u * 4u;
constexpr std::uint32_t kRegNextContextIndex = 0x0607u * 4u;
constexpr std::uint32_t kRegContextKickBase = 0x0650u * 4u;
constexpr std::uint32_t kRegContextLockBase = 0x0690u * 4u;
constexpr std::uint32_t kRegContextClearBase = 0x06A0u * 4u;
constexpr std::uint32_t kContextRegisterGroupBytes = 10u * 4u;

[[nodiscard]] std::uint32_t load_be32(const std::byte* p) noexcept {
  return (std::uint32_t(std::to_integer<std::uint8_t>(p[0])) << 24u) |
         (std::uint32_t(std::to_integer<std::uint8_t>(p[1])) << 16u) |
         (std::uint32_t(std::to_integer<std::uint8_t>(p[2])) << 8u) |
         std::uint32_t(std::to_integer<std::uint8_t>(p[3]));
}

void store_be32(std::byte* p, std::uint32_t v) noexcept {
  p[0] = std::byte((v >> 24u) & 0xFFu);
  p[1] = std::byte((v >> 16u) & 0xFFu);
  p[2] = std::byte((v >> 8u) & 0xFFu);
  p[3] = std::byte(v & 0xFFu);
}

[[nodiscard]] bool read_bit(std::span<const std::byte> data,
                            std::size_t bit_offset,
                            std::uint8_t& out) noexcept {
  if (bit_offset >= data.size() * 8u) return false;
  const auto byte = std::to_integer<std::uint8_t>(data[bit_offset >> 3u]);
  const auto bit = 7u - static_cast<unsigned>(bit_offset & 7u);
  out = static_cast<std::uint8_t>((byte >> bit) & 1u);
  return true;
}

[[nodiscard]] bool read_bits(std::span<const std::byte> data,
                             std::size_t bit_offset, unsigned count,
                             std::uint32_t& out) noexcept {
  if (count > 32 || bit_offset + count > data.size() * 8u) return false;
  out = 0;
  for (unsigned i = 0; i < count; ++i) {
    std::uint8_t bit{};
    if (!read_bit(data, bit_offset + i, bit)) return false;
    out = (out << 1u) | bit;
  }
  return true;
}

void append_bit(std::vector<std::byte>& data, std::size_t bit_offset,
                std::uint8_t bit) {
  const auto byte_index = bit_offset >> 3u;
  if (byte_index >= data.size()) data.resize(byte_index + 1u, std::byte{0});
  if (bit) {
    const auto shift = 7u - static_cast<unsigned>(bit_offset & 7u);
    data[byte_index] |= std::byte(1u << shift);
  }
}

[[nodiscard]] int sample_rate_from_id(std::uint8_t id) noexcept {
  switch (id) {
    case 0: return 24000;
    case 1: return 32000;
    case 2: return 44100;
    case 3: return 48000;
    default: return 0;
  }
}

[[nodiscard]] std::size_t ring_write_capacity(std::size_t capacity,
                                              std::size_t read_offset,
                                              std::size_t write_offset) noexcept {
  if (!capacity) return 0;
  read_offset %= capacity;
  write_offset %= capacity;
  if (read_offset == write_offset) return capacity;
  if (write_offset < read_offset) return read_offset - write_offset;
  return capacity - write_offset + read_offset;
}

struct GatheredFrame {
  std::vector<std::byte> packed_bits{};
  std::uint32_t frame_bits{};
  std::size_t final_packet{};
  std::size_t final_source_bit{};
  bool follows_in_packet{};
};

// XMA packets may interleave streams. For a frame split across packets, the
// packet skip count says how many packets to jump before the continuation.
[[nodiscard]] std::optional<GatheredFrame> gather_frame(
    std::span<const std::byte> input, std::size_t start_bit) {
  if (input.size() < kXmaPacketBytes || start_bit >= input.size() * 8u) {
    return std::nullopt;
  }

  std::uint32_t frame_bits{};
  if (!read_bits(input, start_bit, 15, frame_bits) || frame_bits < 16u ||
      frame_bits >= kMaxFrameBits) {
    return std::nullopt;
  }

  GatheredFrame result{};
  result.frame_bits = frame_bits;
  result.packed_bits.resize((frame_bits + 7u) / 8u, std::byte{0});

  std::size_t source_bit = start_bit;
  std::size_t copied = 0;
  std::size_t packet_index = source_bit / kBitsPerPacket;
  while (copied < frame_bits) {
    if ((packet_index + 1u) * kXmaPacketBytes > input.size()) return std::nullopt;
    const std::size_t packet_start = packet_index * kBitsPerPacket;
    const std::size_t packet_end = packet_start + kBitsPerPacket;
    if (source_bit < packet_start + 32u) source_bit = packet_start + 32u;
    const auto available = packet_end - source_bit;
    const auto take = std::min<std::size_t>(frame_bits - copied, available);
    for (std::size_t i = 0; i < take; ++i) {
      std::uint8_t bit{};
      if (!read_bit(input, source_bit + i, bit)) return std::nullopt;
      append_bit(result.packed_bits, copied + i, bit);
    }
    copied += take;
    source_bit += take;
    if (copied == frame_bits) break;

    const auto header = parse_xma_packet_header(
        input.subspan(packet_index * kXmaPacketBytes, kXmaPacketBytes));
    packet_index += static_cast<std::size_t>(header.skip_count) + 1u;
    if ((packet_index + 1u) * kXmaPacketBytes > input.size()) return std::nullopt;
    source_bit = packet_index * kBitsPerPacket + 32u;
  }

  result.final_packet = packet_index;
  result.final_source_bit = source_bit;
  std::uint8_t follows{};
  if (frame_bits && read_bit(result.packed_bits, frame_bits - 1u, follows)) {
    result.follows_in_packet = follows != 0;
  }
  return result;
}

[[nodiscard]] std::size_t next_packet_frame_offset(
    std::span<const std::byte> input, std::size_t packet_index) noexcept {
  if ((packet_index + 1u) * kXmaPacketBytes > input.size()) return input.size() * 8u;
  const auto packet = input.subspan(packet_index * kXmaPacketBytes, kXmaPacketBytes);
  const auto header = parse_xma_packet_header(packet);
  if (header.first_frame_offset_bits >= kBitsPerPacket) return input.size() * 8u;
  return packet_index * kBitsPerPacket + header.first_frame_offset_bits;
}

[[nodiscard]] std::int16_t float_to_s16(float value) noexcept {
  value = std::clamp(value, -1.0f, 1.0f);
  return static_cast<std::int16_t>(std::lrint(value * 32767.0f));
}


}  // namespace

XmaPacketHeader parse_xma_packet_header(
    std::span<const std::byte> packet) noexcept {
  XmaPacketHeader out{};
  if (packet.size() < 4) return out;
  const auto b0 = std::to_integer<std::uint8_t>(packet[0]);
  const auto b1 = std::to_integer<std::uint8_t>(packet[1]);
  const auto b2 = std::to_integer<std::uint8_t>(packet[2]);
  out.frame_count = static_cast<std::uint8_t>(b0 >> 2u);
  out.first_frame_offset_bits = static_cast<std::uint16_t>(
      (((b0 & 0x3u) << 13u) | (std::uint16_t(b1) << 5u) | (b2 >> 3u)) + 32u);
  out.metadata = static_cast<std::uint8_t>(b2 & 0x7u);
  out.skip_count = std::to_integer<std::uint8_t>(packet[3]);
  return out;
}

XmaContextData XmaContextData::decode(
    std::span<const std::byte, kXmaContextBytes> bytes) noexcept {
  std::array<std::uint32_t, 16> w{};
  for (std::size_t i = 0; i < w.size(); ++i) w[i] = load_be32(bytes.data() + i * 4u);

  XmaContextData d{};
  d.input_buffer_0_packet_count = static_cast<std::uint16_t>(w[0] & 0xFFFu);
  d.loop_count = static_cast<std::uint8_t>((w[0] >> 12u) & 0xFFu);
  d.input_buffer_0_valid = ((w[0] >> 20u) & 1u) != 0;
  d.input_buffer_1_valid = ((w[0] >> 21u) & 1u) != 0;
  d.output_buffer_block_count = static_cast<std::uint8_t>((w[0] >> 22u) & 0x1Fu);
  d.output_buffer_write_offset = static_cast<std::uint8_t>((w[0] >> 27u) & 0x1Fu);

  d.input_buffer_1_packet_count = static_cast<std::uint16_t>(w[1] & 0xFFFu);
  d.loop_subframe_end = static_cast<std::uint8_t>((w[1] >> 12u) & 0x3u);
  d.reserved_dword_1_a = static_cast<std::uint8_t>((w[1] >> 14u) & 0x7u);
  d.loop_subframe_skip = static_cast<std::uint8_t>((w[1] >> 17u) & 0x7u);
  d.subframe_decode_count = static_cast<std::uint8_t>((w[1] >> 20u) & 0xFu);
  d.output_buffer_padding = static_cast<std::uint8_t>((w[1] >> 24u) & 0x7u);
  d.sample_rate = static_cast<std::uint8_t>((w[1] >> 27u) & 0x3u);
  d.is_stereo = ((w[1] >> 29u) & 1u) != 0;
  d.output_buffer_valid = ((w[1] >> 31u) & 1u) != 0;

  d.input_buffer_read_offset = w[2] & 0x03FFFFFFu;
  d.error_status = static_cast<std::uint8_t>((w[2] >> 26u) & 0x1Fu);
  d.error_set = ((w[2] >> 31u) & 1u) != 0;
  d.loop_start = w[3] & 0x03FFFFFFu;
  d.parser_error_status = static_cast<std::uint8_t>((w[3] >> 26u) & 0x1Fu);
  d.parser_error_set = ((w[3] >> 31u) & 1u) != 0;
  d.loop_end = w[4] & 0x03FFFFFFu;
  d.packet_metadata = static_cast<std::uint8_t>((w[4] >> 26u) & 0x1Fu);
  d.current_buffer = ((w[4] >> 31u) & 1u) != 0;
  d.input_buffer_0_ptr = w[5];
  d.input_buffer_1_ptr = w[6];
  d.output_buffer_ptr = w[7];
  d.work_buffer_ptr = w[8];
  d.output_buffer_read_offset = static_cast<std::uint8_t>(w[9] & 0x1Fu);
  d.stop_when_done = ((w[9] >> 30u) & 1u) != 0;
  d.interrupt_when_done = ((w[9] >> 31u) & 1u) != 0;
  for (std::size_t i = 0; i < d.reserved.size(); ++i) d.reserved[i] = w[10 + i];
  return d;
}

void XmaContextData::encode(
    std::span<std::byte, kXmaContextBytes> bytes) const noexcept {
  std::array<std::uint32_t, 16> w{};
  w[0] = (input_buffer_0_packet_count & 0xFFFu) |
         (std::uint32_t(loop_count) << 12u) |
         (std::uint32_t(input_buffer_0_valid) << 20u) |
         (std::uint32_t(input_buffer_1_valid) << 21u) |
         ((std::uint32_t(output_buffer_block_count) & 0x1Fu) << 22u) |
         ((std::uint32_t(output_buffer_write_offset) & 0x1Fu) << 27u);
  w[1] = (input_buffer_1_packet_count & 0xFFFu) |
         ((std::uint32_t(loop_subframe_end) & 0x3u) << 12u) |
         ((std::uint32_t(reserved_dword_1_a) & 0x7u) << 14u) |
         ((std::uint32_t(loop_subframe_skip) & 0x7u) << 17u) |
         ((std::uint32_t(subframe_decode_count) & 0xFu) << 20u) |
         ((std::uint32_t(output_buffer_padding) & 0x7u) << 24u) |
         ((std::uint32_t(sample_rate) & 0x3u) << 27u) |
         (std::uint32_t(is_stereo) << 29u) |
         (std::uint32_t(output_buffer_valid) << 31u);
  w[2] = (input_buffer_read_offset & 0x03FFFFFFu) |
         ((std::uint32_t(error_status) & 0x1Fu) << 26u) |
         (std::uint32_t(error_set) << 31u);
  w[3] = (loop_start & 0x03FFFFFFu) |
         ((std::uint32_t(parser_error_status) & 0x1Fu) << 26u) |
         (std::uint32_t(parser_error_set) << 31u);
  w[4] = (loop_end & 0x03FFFFFFu) |
         ((std::uint32_t(packet_metadata) & 0x1Fu) << 26u) |
         (std::uint32_t(current_buffer) << 31u);
  w[5] = input_buffer_0_ptr;
  w[6] = input_buffer_1_ptr;
  w[7] = output_buffer_ptr;
  w[8] = work_buffer_ptr;
  w[9] = (std::uint32_t(output_buffer_read_offset) & 0x1Fu) |
         (std::uint32_t(stop_when_done) << 30u) |
         (std::uint32_t(interrupt_when_done) << 31u);
  for (std::size_t i = 0; i < reserved.size(); ++i) w[10 + i] = reserved[i];
  for (std::size_t i = 0; i < w.size(); ++i) store_be32(bytes.data() + i * 4u, w[i]);
}

struct XmaDecoder::Runtime {
  static constexpr std::size_t kMaxFrameBytes =
      kXmaSamplesPerFrame * 2u * sizeof(std::int16_t);
  static constexpr std::size_t kDecoderStartPaddingSamples = 192;

  mutable std::mutex mutex{};
  std::condition_variable cv{};
  bool enabled{};
  bool busy{};
  // Set by XMADisableContext while a decode is in flight. The worker must not
  // re-arm the context when finishing that decode.
  bool disable_requested{};
  std::atomic<std::uint64_t> decoded_samples{0};
  const AVCodec* codec{};
  AVCodecContext* codec_context{};
  AVPacket* packet{};
  AVFrame* frame{};
  int configured_sample_rate{};
  int configured_channels{};

  // FFmpeg's XMAFRAMES output is shifted by the codec's start padding. Keep
  // the tail of one decoded frame and combine it with the head of the next so
  // guest-visible PCM is aligned to Xbox frame/sample numbering.
  std::array<std::byte, kMaxFrameBytes> carry_tail{};
  std::size_t carry_tail_bytes{};
  bool carry_valid{};
  std::uint8_t carry_output_limit_blocks{};
  std::uint8_t carry_start_skip_blocks{};
  bool loop_start_skip_pending{};
  bool completion_notified{};

  // One assembled Xbox frame may be consumed over several hardware kicks
  // because subframe_decode_count is expressed in 256-byte output blocks.
  std::array<std::byte, kMaxFrameBytes> pending_pcm{};
  std::size_t pending_bytes{};
  std::size_t pending_offset{};

  void reset_stream() noexcept {
    enabled = false;
    disable_requested = false;
    decoded_samples.store(0, std::memory_order_relaxed);
    configured_sample_rate = 0;
    configured_channels = 0;
    if (codec_context) avcodec_free_context(&codec_context);
    if (packet) av_packet_unref(packet);
    if (frame) av_frame_unref(frame);
    carry_tail.fill(std::byte{0});
    carry_tail_bytes = 0;
    carry_valid = false;
    carry_output_limit_blocks = 0;
    carry_start_skip_blocks = 0;
    loop_start_skip_pending = false;
    completion_notified = false;
    pending_pcm.fill(std::byte{0});
    pending_bytes = 0;
    pending_offset = 0;
  }

  ~Runtime() {
    if (packet) av_packet_free(&packet);
    if (frame) av_frame_free(&frame);
    if (codec_context) avcodec_free_context(&codec_context);
  }
};

struct XmaDecoder::MmioBridge {
  std::mutex mutex{};
  XmaDecoder* owner{};
};

XmaDecoder::XmaDecoder(memory::AddressSpace& memory)
    : memory_(memory), mmio_bridge_(std::make_shared<MmioBridge>()) {
  for (auto& value : allocated_) value.store(false);
}

XmaDecoder::~XmaDecoder() { shutdown(); }

bool XmaDecoder::initialize(std::string* error) {
  if (initialized_.load()) return true;
  if (!avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES)) {
    if (error) *error = "FFmpeg was built without the XMAFRAMES decoder";
    return false;
  }

  memory::PhysicalAllocationOptions options{};
  options.page_class = memory::PhysicalPageClass::Page64K;
  options.alignment = 256;
  options.top_down = true;
  options.zero_initialize = true;
  const auto requested_size = static_cast<std::uint32_t>(
      kXmaContextCount * kXmaContextBytes);
  if (!memory_.allocate_physical(requested_size, options, context_physical_base_)) {
    if (error) *error = "unable to allocate Xbox XMA context array";
    return false;
  }
  context_allocation_size_ = requested_size;
  const auto alias = memory::AddressSpace::physical_guest_alias(
      context_physical_base_, options.page_class);
  if (!alias) {
    (void)memory_.free_physical(context_physical_base_, context_allocation_size_);
    context_physical_base_ = 0;
    if (error) *error = "XMA context allocation has no Xbox-visible physical alias";
    return false;
  }
  context_guest_base_ = *alias;
  context_page_class_ = options.page_class;

  {
    std::lock_guard lock(mmio_mutex_);
    mmio_registers_.fill(0);
    mmio_registers_[kRegContextArrayAddress / 4u] = context_physical_base_;
    next_context_index_ = 1;
  }
  {
    std::lock_guard bridge_lock(mmio_bridge_->mutex);
    mmio_bridge_->owner = this;
  }
  if (!mmio_registered_) {
    const auto bridge = mmio_bridge_;
    if (!memory_.add_mmio_range(
            kXmaMmioBase, kXmaMmioSize,
            [bridge](cpu::GuestAddress address, std::uint32_t width) {
              std::lock_guard bridge_lock(bridge->mutex);
              if (bridge->owner) return bridge->owner->mmio_read(address, width);
              return std::uint64_t{0};
            },
            [bridge](cpu::GuestAddress address, std::uint32_t width,
                     std::uint64_t value) {
              std::lock_guard bridge_lock(bridge->mutex);
              if (bridge->owner) bridge->owner->mmio_write(address, width, value);
            },
            "Xenon XMA")) {
      {
        std::lock_guard bridge_lock(mmio_bridge_->mutex);
        mmio_bridge_->owner = nullptr;
      }
      (void)memory_.free_physical(context_physical_base_, context_allocation_size_);
      context_physical_base_ = 0;
      context_guest_base_ = 0;
      context_allocation_size_ = 0;
      if (error) *error = "unable to register Xbox XMA MMIO aperture";
      return false;
    }
    mmio_registered_ = true;
  }

  for (auto& value : allocated_) value.store(false);
  for (auto& runtime : runtimes_) {
    if (!runtime) runtime = std::make_unique<Runtime>();
    std::lock_guard lock(runtime->mutex);
    runtime->reset_stream();
    runtime->busy = false;
  }

  worker_running_.store(true);
  worker_ = std::thread(&XmaDecoder::worker_main, this);
  initialized_.store(true);
  return true;
}

void XmaDecoder::shutdown() noexcept {
  if (!initialized_.exchange(false)) return;
  if (mmio_bridge_) {
    std::lock_guard bridge_lock(mmio_bridge_->mutex);
    mmio_bridge_->owner = nullptr;
  }
  worker_running_.store(false);
  worker_cv_.notify_all();
  if (worker_.joinable()) worker_.join();
  for (std::size_t i = 0; i < kXmaContextCount; ++i) {
    allocated_[i].store(false);
    runtimes_[i].reset();
  }
  if (context_physical_base_) {
    (void)memory_.free_physical(context_physical_base_, context_allocation_size_);
  }
  context_physical_base_ = 0;
  context_guest_base_ = 0;
  context_allocation_size_ = 0;
}

std::optional<std::size_t> XmaDecoder::index_of(
    cpu::GuestAddress context) const noexcept {
  if (!context_guest_base_ || context < context_guest_base_) return std::nullopt;
  const auto offset = context - context_guest_base_;
  if ((offset & (kXmaContextBytes - 1u)) != 0 ||
      offset >= kXmaContextCount * kXmaContextBytes) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(offset / kXmaContextBytes);
}

bool XmaDecoder::owns_context(cpu::GuestAddress context) const noexcept {
  const auto index = index_of(context);
  return index && allocated_[*index].load();
}

cpu::GuestAddress XmaDecoder::allocate_context() {
  if (!initialized_.load()) return 0;
  for (std::size_t i = 0; i < kXmaContextCount; ++i) {
    bool expected = false;
    if (allocated_[i].compare_exchange_strong(expected, true)) {
      auto& runtime = *runtimes_[i];
      {
        std::lock_guard lock(runtime.mutex);
        runtime.reset_stream();
        runtime.busy = false;
      }
      std::array<std::byte, kXmaContextBytes> zero{};
      if (!memory_.write_physical(
              context_physical_base_ + static_cast<std::uint32_t>(i * kXmaContextBytes),
              zero)) {
        allocated_[i].store(false);
        return 0;
      }
      return context_guest_base_ + static_cast<cpu::GuestAddress>(i * kXmaContextBytes);
    }
  }
  return 0;
}

bool XmaDecoder::release_context(cpu::GuestAddress context) {
  const auto index = index_of(context);
  if (!index || !allocated_[*index].load()) return false;
  auto* runtime = runtimes_[*index].get();
  if (runtime) {
    std::unique_lock lock(runtime->mutex);
    runtime->disable_requested = true;
    runtime->enabled = false;
    runtime->cv.wait(lock, [&] { return !runtime->busy; });
  }
  std::array<std::byte, kXmaContextBytes> zero{};
  if (!memory_.write_physical(
          context_physical_base_ + static_cast<std::uint32_t>(*index * kXmaContextBytes),
          zero)) {
    return false;
  }
  if (runtime) {
    std::lock_guard lock(runtime->mutex);
    runtime->reset_stream();
    runtime->busy = false;
  }
  allocated_[*index].store(false);
  return true;
}

bool XmaDecoder::read_context(cpu::GuestAddress context, XmaContextData& out) const {
  const auto index = index_of(context);
  if (!index || !allocated_[*index].load()) return false;
  std::array<std::byte, kXmaContextBytes> bytes{};
  if (!memory_.copy_physical_range(
          context_physical_base_ + static_cast<std::uint32_t>(*index * kXmaContextBytes),
          bytes)) {
    return false;
  }
  out = XmaContextData::decode(bytes);
  return true;
}

bool XmaDecoder::write_context(cpu::GuestAddress context,
                               const XmaContextData& data,
                               bool external_write) {
  const auto index = index_of(context);
  if (!index || !allocated_[*index].load()) return false;
  std::array<std::byte, kXmaContextBytes> bytes{};
  data.encode(bytes);
  if (external_write) {
    return memory_.write_physical(
        context_physical_base_ + static_cast<std::uint32_t>(*index * kXmaContextBytes),
        bytes);
  }
  memory_.write_bytes(context, bytes);
  return true;
}

bool XmaDecoder::resolve_guest_physical(cpu::GuestAddress guest,
                                        std::uint32_t& physical) const {
  if (!guest) {
    physical = 0;
    return true;
  }
  physical = memory_.get_physical_address(guest);
  return physical != 0xFFFFFFFFu;
}

bool XmaDecoder::initialize_context(cpu::GuestAddress context,
                                    const XmaContextInit& init) {
  if (!owns_context(context) || init.input_buffer_0_packet_count > 0xFFFu ||
      init.input_buffer_1_packet_count > 0xFFFu ||
      init.output_buffer_block_count == 0 || init.output_buffer_block_count > 31u ||
      init.channel_count > 1u || init.sample_rate > 3u ||
      init.subframe_decode_count > 8u || init.loop_subframe_end > 3u ||
      init.loop_subframe_skip > 7u) {
    return false;
  }

  std::uint32_t in0{}, in1{}, out{}, work{};
  if (!resolve_guest_physical(init.input_buffer_0, in0) ||
      !resolve_guest_physical(init.input_buffer_1, in1) ||
      !resolve_guest_physical(init.output_buffer, out) || !out ||
      !resolve_guest_physical(init.work_buffer, work)) {
    return false;
  }

  const auto runtime_index = index_of(context);
  if (!runtime_index || !runtimes_[*runtime_index]) return false;
  {
    auto& runtime = *runtimes_[*runtime_index];
    std::unique_lock lock(runtime.mutex);
    runtime.disable_requested = true;
    runtime.enabled = false;
    runtime.cv.wait(lock, [&] { return !runtime.busy; });
    runtime.reset_stream();
  }

  XmaContextData data{};
  data.input_buffer_0_ptr = in0;
  data.input_buffer_0_packet_count =
      static_cast<std::uint16_t>(init.input_buffer_0_packet_count);
  data.input_buffer_1_ptr = in1;
  data.input_buffer_1_packet_count =
      static_cast<std::uint16_t>(init.input_buffer_1_packet_count);
  data.input_buffer_read_offset = init.input_buffer_read_offset & 0x03FFFFFFu;
  data.output_buffer_ptr = out;
  data.output_buffer_block_count =
      static_cast<std::uint8_t>(init.output_buffer_block_count);
  data.work_buffer_ptr = work;
  data.subframe_decode_count = static_cast<std::uint8_t>(init.subframe_decode_count);
  // Xbox XMA_CONTEXT_INIT encodes channel mode, not a literal count: 0 is
  // mono and 1 is stereo. This matches the hardware-facing XDK/Xenia path.
  data.is_stereo = init.channel_count != 0u;
  data.sample_rate = static_cast<std::uint8_t>(init.sample_rate);
  data.loop_start = init.loop_start & 0x03FFFFFFu;
  data.loop_end = init.loop_end & 0x03FFFFFFu;
  data.loop_count = init.loop_count;
  data.loop_subframe_end = init.loop_subframe_end;
  data.loop_subframe_skip = init.loop_subframe_skip;
  return write_context(context, data, false);
}

bool XmaDecoder::enable_context(cpu::GuestAddress context) {
  const auto index = index_of(context);
  if (!index || !allocated_[*index].load() || !runtimes_[*index]) return false;
  {
    std::lock_guard lock(runtimes_[*index]->mutex);
    runtimes_[*index]->disable_requested = false;
    runtimes_[*index]->completion_notified = false;
    runtimes_[*index]->enabled = true;
  }
  worker_cv_.notify_one();
  return true;
}

bool XmaDecoder::disable_context(cpu::GuestAddress context, bool wait) {
  const auto index = index_of(context);
  if (!index || !allocated_[*index].load() || !runtimes_[*index]) return false;
  auto& runtime = *runtimes_[*index];
  std::unique_lock lock(runtime.mutex);
  runtime.disable_requested = true;
  runtime.enabled = false;
  if (!runtime.busy) return true;
  if (!wait) return false;
  runtime.cv.wait(lock, [&] { return !runtime.busy; });
  return true;
}

bool XmaDecoder::clear_context(cpu::GuestAddress context) {
  const auto index = index_of(context);
  if (!index || !allocated_[*index].load() || !runtimes_[*index]) return false;

  auto& runtime = *runtimes_[*index];
  std::unique_lock lock(runtime.mutex);
  runtime.disable_requested = true;
  runtime.enabled = false;
  runtime.cv.wait(lock, [&] { return !runtime.busy; });
  runtime.reset_stream();
  runtime.busy = false;
  lock.unlock();

  XmaContextData data{};
  if (!read_context(context, data)) return false;
  data.input_buffer_0_valid = false;
  data.input_buffer_1_valid = false;
  data.output_buffer_valid = false;
  data.input_buffer_read_offset = 32u;
  data.output_buffer_read_offset = 0;
  data.output_buffer_write_offset = 0;
  return write_context(context, data, true);
}

void XmaDecoder::set_completion_sink(CompletionSink sink) {
  std::lock_guard lock(completion_mutex_);
  completion_sink_ = std::move(sink);
}

void XmaDecoder::notify_completion(cpu::GuestAddress context,
                                   bool interrupt_requested) {
  CompletionSink sink;
  {
    std::lock_guard lock(completion_mutex_);
    sink = completion_sink_;
  }
  if (sink) sink(context, interrupt_requested);
}

bool XmaDecoder::block_while_in_use(cpu::GuestAddress context) {
  const auto index = index_of(context);
  if (!index || !allocated_[*index].load() || !runtimes_[*index]) return false;
  auto& runtime = *runtimes_[*index];
  std::unique_lock lock(runtime.mutex);
  runtime.cv.wait(lock, [&] { return !runtime.busy && !runtime.enabled; });
  return true;
}

std::uint64_t XmaDecoder::decoded_samples(cpu::GuestAddress context) const {
  const auto index = index_of(context);
  if (!index || !allocated_[*index].load() || !runtimes_[*index]) return 0;
  return runtimes_[*index]->decoded_samples.load(std::memory_order_relaxed);
}

bool XmaDecoder::merge_hardware_progress(
    cpu::GuestAddress context, const XmaContextData& initial,
    const XmaContextData& progressed) {
  XmaContextData fresh{};
  if (!read_context(context, fresh)) return false;

  // Only publish fields owned by the asynchronous XMA engine. Guest setters
  // may legally update the read offset, valid flags, buffer pointers, counts,
  // or output read pointer while decode is in flight; rewriting the full stale
  // context would lose those CPU-originated updates.
  fresh.loop_count = progressed.loop_count;
  fresh.output_buffer_write_offset = progressed.output_buffer_write_offset;
  fresh.input_buffer_read_offset = progressed.input_buffer_read_offset;
  fresh.packet_metadata = progressed.packet_metadata;
  fresh.current_buffer = progressed.current_buffer;
  fresh.error_status = progressed.error_status;
  fresh.error_set = progressed.error_set;
  fresh.parser_error_status = progressed.parser_error_status;
  fresh.parser_error_set = progressed.parser_error_set;

  if (initial.input_buffer_0_valid && !progressed.input_buffer_0_valid) {
    fresh.input_buffer_0_valid = false;
  }
  if (initial.input_buffer_1_valid && !progressed.input_buffer_1_valid) {
    fresh.input_buffer_1_valid = false;
  }
  if (initial.output_buffer_valid && !progressed.output_buffer_valid) {
    fresh.output_buffer_valid = false;
  }

  return write_context(context, fresh, true);
}

bool XmaDecoder::decode_one(std::size_t index) {
  if (index >= kXmaContextCount || !allocated_[index].load() ||
      !runtimes_[index]) {
    return false;
  }

  auto& runtime = *runtimes_[index];
  std::unique_lock runtime_lock(runtime.mutex);
  if (!runtime.enabled || runtime.busy) return false;
  runtime.enabled = false;
  runtime.busy = true;
  runtime_lock.unlock();

  const cpu::GuestAddress context =
      context_guest_base_ + static_cast<cpu::GuestAddress>(index * kXmaContextBytes);
  const auto finish = [&](bool continue_work = false, bool completed = false,
                          bool interrupt_requested = false) {
    bool publish_completion = false;
    {
      std::lock_guard lock(runtime.mutex);
      runtime.enabled = continue_work && !runtime.disable_requested;
      runtime.busy = false;
      if (completed && !runtime.completion_notified) {
        runtime.completion_notified = true;
        publish_completion = true;
      }
      runtime.cv.notify_all();
    }
    if (publish_completion) notify_completion(context, interrupt_requested);
  };
  XmaContextData data{};
  if (!read_context(context, data)) {
    finish();
    return false;
  }
  const XmaContextData initial = data;

  if (!data.output_buffer_valid || !data.output_buffer_ptr ||
      !data.output_buffer_block_count) {
    finish();
    return false;
  }

  const int channels = data.is_stereo ? 2 : 1;
  const auto capacity = static_cast<std::size_t>(data.output_buffer_block_count) *
                        kXmaOutputBlockBytes;
  const auto read_offset = static_cast<std::size_t>(data.output_buffer_read_offset) *
                           kXmaOutputBlockBytes;
  const auto write_offset = static_cast<std::size_t>(data.output_buffer_write_offset) *
                            kXmaOutputBlockBytes;
  const auto writable_bytes = ring_write_capacity(capacity, read_offset, write_offset);
  const auto writable_blocks = writable_bytes / kXmaOutputBlockBytes;
  const auto decode_blocks = std::clamp<std::size_t>(
      data.subframe_decode_count ? data.subframe_decode_count : 1u, 1u, 8u);

  // If a decoded Xbox frame was only partially copied on a prior kick, consume
  // another bounded block window before touching the compressed stream again.
  if (runtime.pending_offset < runtime.pending_bytes) {
    const auto pending_blocks =
        (runtime.pending_bytes - runtime.pending_offset) / kXmaOutputBlockBytes;
    const auto blocks_to_write = std::min(decode_blocks, pending_blocks);
    const auto finishing_frame = blocks_to_write == pending_blocks;
    const auto headroom = finishing_frame
                              ? static_cast<std::size_t>(data.output_buffer_padding)
                              : 0u;
    if (!blocks_to_write || writable_blocks < blocks_to_write + headroom) {
      finish();
      return false;
    }

    const auto bytes_to_write = blocks_to_write * kXmaOutputBlockBytes;
    const auto write_at = write_offset % capacity;
    const auto first_write = std::min(bytes_to_write, capacity - write_at);
    const std::span<const std::byte> source(runtime.pending_pcm.data() +
                                                runtime.pending_offset,
                                            bytes_to_write);
    if (!memory_.write_physical(
            data.output_buffer_ptr + static_cast<std::uint32_t>(write_at),
            source.first(first_write)) ||
        (first_write < bytes_to_write &&
         !memory_.write_physical(data.output_buffer_ptr,
                                 source.subspan(first_write)))) {
      data.error_status = 9;
      data.error_set = true;
      (void)merge_hardware_progress(context, initial, data);
      finish();
      return false;
    }

    runtime.pending_offset += bytes_to_write;
    if (runtime.pending_offset == runtime.pending_bytes) {
      runtime.pending_offset = 0;
      runtime.pending_bytes = 0;
    }

    const auto new_write = (write_offset + bytes_to_write) % capacity;
    data.output_buffer_write_offset =
        static_cast<std::uint8_t>(new_write / kXmaOutputBlockBytes);
    if (new_write == (read_offset % capacity)) data.output_buffer_valid = false;
    runtime.decoded_samples.fetch_add(
        bytes_to_write /
            (static_cast<std::size_t>(channels) * sizeof(std::int16_t)),
        std::memory_order_relaxed);

    const bool published = merge_hardware_progress(context, initial, data);
    const bool input_available = data.input_buffer_0_valid || data.input_buffer_1_valid;
    const bool pending = runtime.pending_offset < runtime.pending_bytes;
    const bool continue_work =
        published && data.output_buffer_valid && (pending || input_available);
    const bool completed = published && !pending && !input_available;
    finish(continue_work, completed, data.interrupt_when_done);
    return published;
  }

  // Hardware does not advance compressed input unless enough output-ring
  // space exists for the requested block quantum plus its reserved padding.
  if (writable_blocks < decode_blocks + data.output_buffer_padding) {
    finish();
    return false;
  }

  if (!data.input_buffer_0_valid && !data.input_buffer_1_valid) {
    data.output_buffer_valid = false;
    const bool published = merge_hardware_progress(context, initial, data);
    finish(false, published, data.interrupt_when_done);
    return published;
  }

  bool current = data.current_buffer;
  auto current_valid = current ? data.input_buffer_1_valid : data.input_buffer_0_valid;
  if (!current_valid) {
    data.current_buffer = !data.current_buffer;
    data.input_buffer_read_offset = 32u;
    current = data.current_buffer;
    current_valid = current ? data.input_buffer_1_valid : data.input_buffer_0_valid;
    if (!current_valid) {
      data.output_buffer_valid = false;
      const bool published = merge_hardware_progress(context, initial, data);
      finish(false, published, data.interrupt_when_done);
      return published;
    }
  }

  const auto input_physical = current ? data.input_buffer_1_ptr : data.input_buffer_0_ptr;
  const auto packet_count = current ? data.input_buffer_1_packet_count
                                    : data.input_buffer_0_packet_count;
  if (!input_physical || !packet_count) {
    if (current) data.input_buffer_1_valid = false;
    else data.input_buffer_0_valid = false;
    data.current_buffer = !data.current_buffer;
    data.input_buffer_read_offset = 32u;
    const bool published = merge_hardware_progress(context, initial, data);
    const bool input_available = data.input_buffer_0_valid || data.input_buffer_1_valid;
    const bool continue_work = published && data.output_buffer_valid && input_available;
    finish(continue_work, published && !input_available, data.interrupt_when_done);
    return published;
  }

  const auto current_input_bytes =
      static_cast<std::size_t>(packet_count) * kXmaPacketBytes;
  std::vector<std::byte> input(current_input_bytes);
  if (!memory_.copy_physical_range(input_physical, input)) {
    data.error_status = 1;
    data.error_set = true;
    (void)merge_hardware_progress(context, initial, data);
    finish();
    return false;
  }

  // Rolling XMA streams may split a frame across input buffer 0/1. Copy the
  // next valid buffer after the current one into a logical packet sequence so
  // frame gathering can cross the physical discontinuity without assuming the
  // guest buffers themselves are contiguous.
  const bool next_index = !current;
  const bool next_valid = next_index ? data.input_buffer_1_valid
                                     : data.input_buffer_0_valid;
  const auto next_physical = next_index ? data.input_buffer_1_ptr
                                        : data.input_buffer_0_ptr;
  const auto next_packets = next_index ? data.input_buffer_1_packet_count
                                       : data.input_buffer_0_packet_count;
  if (next_valid && next_physical && next_packets) {
    const auto old_size = input.size();
    input.resize(old_size + static_cast<std::size_t>(next_packets) * kXmaPacketBytes);
    if (!memory_.copy_physical_range(
            next_physical,
            std::span<std::byte>(input).subspan(old_size))) {
      data.error_status = 1;
      data.error_set = true;
      (void)merge_hardware_progress(context, initial, data);
      finish();
      return false;
    }
  }

  const auto loop_start = std::max<std::uint32_t>(32u, data.loop_start);
  const auto loop_end = std::max<std::uint32_t>(32u, data.loop_end);
  std::size_t start_bit = std::max<std::uint32_t>(32u, data.input_buffer_read_offset);
  if ((start_bit % kBitsPerPacket) == 0) {
    start_bit = next_packet_frame_offset(input, start_bit / kBitsPerPacket);
  }
  if (start_bit >= input.size() * 8u) {
    if (current) data.input_buffer_1_valid = false;
    else data.input_buffer_0_valid = false;
    data.current_buffer = !data.current_buffer;
    data.input_buffer_read_offset = 32u;
    const bool published = merge_hardware_progress(context, initial, data);
    const bool input_available = data.input_buffer_0_valid || data.input_buffer_1_valid;
    const bool continue_work = published && data.output_buffer_valid && input_available;
    finish(continue_work, published && !input_available, data.interrupt_when_done);
    return published;
  }

  const bool is_loop_end_frame =
      data.loop_count != 0 && loop_start < loop_end && start_bit == loop_end;
  const auto start_packet = start_bit / kBitsPerPacket;
  const auto packet_header = parse_xma_packet_header(
      std::span<const std::byte>(input).subspan(start_packet * kXmaPacketBytes,
                                                kXmaPacketBytes));
  data.packet_metadata = packet_header.metadata;

  const auto gathered = gather_frame(input, start_bit);
  if (!gathered) {
    data.parser_error_status = 1;
    data.parser_error_set = true;
    (void)merge_hardware_progress(context, initial, data);
    finish();
    return false;
  }

  const int sample_rate = sample_rate_from_id(data.sample_rate);
  if (!sample_rate) {
    data.error_status = 2;
    data.error_set = true;
    (void)merge_hardware_progress(context, initial, data);
    finish();
    return false;
  }

  if (!runtime.codec) runtime.codec = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  if (!runtime.codec) {
    data.error_status = 3;
    data.error_set = true;
    (void)merge_hardware_progress(context, initial, data);
    finish();
    return false;
  }
  if (!runtime.codec_context || runtime.configured_sample_rate != sample_rate ||
      runtime.configured_channels != channels) {
    if (runtime.codec_context) avcodec_free_context(&runtime.codec_context);
    runtime.codec_context = avcodec_alloc_context3(runtime.codec);
    if (!runtime.codec_context) {
      data.error_status = 4;
      data.error_set = true;
      (void)merge_hardware_progress(context, initial, data);
      finish();
      return false;
    }
    runtime.codec_context->sample_rate = sample_rate;
#if defined(XENON_AUDIO_FFMPEG_HAS_CH_LAYOUT)
    av_channel_layout_default(&runtime.codec_context->ch_layout, channels);
#else
    runtime.codec_context->channels = channels;
#endif
    if (avcodec_open2(runtime.codec_context, runtime.codec, nullptr) < 0) {
      data.error_status = 5;
      data.error_set = true;
      (void)merge_hardware_progress(context, initial, data);
      finish();
      return false;
    }
    runtime.configured_sample_rate = sample_rate;
    runtime.configured_channels = channels;
    runtime.carry_valid = false;
    runtime.carry_tail_bytes = 0;
  }
  if (!runtime.packet) runtime.packet = av_packet_alloc();
  if (!runtime.frame) runtime.frame = av_frame_alloc();
  if (!runtime.packet || !runtime.frame) {
    data.error_status = 6;
    data.error_set = true;
    (void)merge_hardware_progress(context, initial, data);
    finish();
    return false;
  }

  av_packet_unref(runtime.packet);
  const auto payload_bytes = (gathered->frame_bits + 7u) / 8u;
  if (av_new_packet(runtime.packet, static_cast<int>(payload_bytes + 1u)) < 0) {
    data.error_status = 7;
    data.error_set = true;
    (void)merge_hardware_progress(context, initial, data);
    finish();
    return false;
  }
  const auto end_padding =
      static_cast<std::uint8_t>((8u - (gathered->frame_bits & 7u)) & 7u);
  runtime.packet->data[0] = static_cast<std::uint8_t>(end_padding << 2u);
  std::memcpy(runtime.packet->data + 1, gathered->packed_bits.data(), payload_bytes);

  av_frame_unref(runtime.frame);
  const int send_result = avcodec_send_packet(runtime.codec_context, runtime.packet);
  const int receive_result =
      send_result < 0 ? send_result
                      : avcodec_receive_frame(runtime.codec_context, runtime.frame);
  if (receive_result < 0 ||
      runtime.frame->nb_samples < static_cast<int>(kXmaSamplesPerFrame) ||
      runtime.frame->format != AV_SAMPLE_FMT_FLTP) {
    data.error_status = 8;
    data.error_set = true;
    (void)merge_hardware_progress(context, initial, data);
    finish();
    return false;
  }

  const auto frame_bytes = static_cast<std::size_t>(kXmaSamplesPerFrame) *
                           channels * sizeof(std::int16_t);
  std::array<std::byte, Runtime::kMaxFrameBytes> decoded_pcm{};
  std::size_t decoded_offset = 0;
  for (std::uint32_t sample = 0; sample < kXmaSamplesPerFrame; ++sample) {
    for (int ch = 0; ch < channels; ++ch) {
      const auto* plane =
          reinterpret_cast<const float*>(runtime.frame->extended_data[ch]);
      const auto value = static_cast<std::uint16_t>(float_to_s16(plane[sample]));
      decoded_pcm[decoded_offset++] = std::byte((value >> 8u) & 0xFFu);
      decoded_pcm[decoded_offset++] = std::byte(value & 0xFFu);
    }
  }

  const auto lead_bytes = Runtime::kDecoderStartPaddingSamples *
                          static_cast<std::size_t>(channels) * sizeof(std::int16_t);
  const auto tail_bytes = frame_bytes - lead_bytes;
  if (runtime.carry_valid && runtime.carry_tail_bytes == tail_bytes) {
    std::array<std::byte, Runtime::kMaxFrameBytes> aligned{};
    std::memcpy(aligned.data(), runtime.carry_tail.data(), tail_bytes);
    std::memcpy(aligned.data() + tail_bytes, decoded_pcm.data(), lead_bytes);

    const auto total_blocks = frame_bytes / kXmaOutputBlockBytes;
    const auto start_block = std::min<std::size_t>(runtime.carry_start_skip_blocks,
                                                   total_blocks);
    const auto end_block = runtime.carry_output_limit_blocks
                               ? std::min<std::size_t>(
                                     runtime.carry_output_limit_blocks, total_blocks)
                               : total_blocks;
    runtime.pending_offset = 0;
    runtime.pending_bytes = 0;
    if (end_block > start_block) {
      runtime.pending_bytes = (end_block - start_block) * kXmaOutputBlockBytes;
      std::memcpy(runtime.pending_pcm.data(),
                  aligned.data() + start_block * kXmaOutputBlockBytes,
                  runtime.pending_bytes);
    }
  }

  std::memcpy(runtime.carry_tail.data(), decoded_pcm.data() + lead_bytes, tail_bytes);
  runtime.carry_tail_bytes = tail_bytes;
  runtime.carry_valid = true;
  runtime.carry_output_limit_blocks = is_loop_end_frame
      ? static_cast<std::uint8_t>((data.loop_subframe_end + 1u) * channels)
      : 0u;
  runtime.carry_start_skip_blocks = runtime.loop_start_skip_pending
      ? static_cast<std::uint8_t>(data.loop_subframe_skip * channels)
      : 0u;
  runtime.loop_start_skip_pending = false;

  std::size_t next_bit{};
  if (gathered->follows_in_packet &&
      gathered->final_source_bit < (gathered->final_packet + 1u) * kBitsPerPacket) {
    next_bit = gathered->final_source_bit;
  } else {
    const auto final_packet_span = std::span<const std::byte>(input).subspan(
        gathered->final_packet * kXmaPacketBytes, kXmaPacketBytes);
    const auto final_header = parse_xma_packet_header(final_packet_span);
    const auto next_packet = gathered->final_packet +
                             static_cast<std::size_t>(final_header.skip_count) + 1u;
    next_bit = next_packet_frame_offset(input, next_packet);
  }

  if (is_loop_end_frame ||
      (data.loop_count && loop_start < loop_end && next_bit > loop_end)) {
    next_bit = loop_start;
    if (data.loop_count != 255) --data.loop_count;
    runtime.loop_start_skip_pending = true;
  }

  const auto current_input_bits = current_input_bytes * 8u;
  if (next_bit >= input.size() * 8u) {
    if (current) data.input_buffer_1_valid = false;
    else data.input_buffer_0_valid = false;
    if (input.size() > current_input_bytes) {
      if (next_index) data.input_buffer_1_valid = false;
      else data.input_buffer_0_valid = false;
    }
    data.current_buffer = !current;
    data.input_buffer_read_offset = 32u;
  } else if (next_bit >= current_input_bits) {
    if (current) data.input_buffer_1_valid = false;
    else data.input_buffer_0_valid = false;
    data.current_buffer = !current;
    data.input_buffer_read_offset =
        static_cast<std::uint32_t>(next_bit - current_input_bits) & 0x03FFFFFFu;
  } else {
    data.input_buffer_read_offset =
        static_cast<std::uint32_t>(next_bit) & 0x03FFFFFFu;
  }

  const bool published = merge_hardware_progress(context, initial, data);
  const bool input_available = data.input_buffer_0_valid || data.input_buffer_1_valid;
  const bool pending = runtime.pending_offset < runtime.pending_bytes;
  const bool continue_work =
      published && data.output_buffer_valid && (pending || input_available);
  const bool completed = published && !pending && !input_available;
  finish(continue_work, completed, data.interrupt_when_done);
  return published;
}

std::uint64_t XmaDecoder::mmio_read(cpu::GuestAddress address,
                                    std::uint32_t width) {
  if (address < kXmaMmioBase || address >= kXmaMmioBase + kXmaMmioSize ||
      (width != 1u && width != 2u && width != 4u)) {
    return 0;
  }
  const auto offset = static_cast<std::uint32_t>(address - kXmaMmioBase);
  const auto reg_offset = offset & ~3u;
  std::uint32_t value{};
  {
    std::lock_guard lock(mmio_mutex_);
    if (reg_offset == kRegContextArrayAddress) {
      value = context_physical_base_;
    } else if (reg_offset == kRegCurrentContextIndex) {
      value = next_context_index_;
      next_context_index_ = (next_context_index_ + 1u) % kXmaContextCount;
      mmio_registers_[kRegCurrentContextIndex / 4u] = value;
      mmio_registers_[kRegNextContextIndex / 4u] = next_context_index_;
    } else if (reg_offset == kRegNextContextIndex) {
      value = next_context_index_;
    } else {
      value = mmio_registers_[reg_offset / 4u];
    }
  }

  const auto byte_in_reg = offset & 3u;
  if (width == 4u && byte_in_reg == 0) return value;
  if (width == 2u && byte_in_reg <= 2u) {
    const auto shift = (2u - byte_in_reg) * 8u;
    return (value >> shift) & 0xFFFFu;
  }
  if (width == 1u) {
    const auto shift = (3u - byte_in_reg) * 8u;
    return (value >> shift) & 0xFFu;
  }
  return 0;
}

void XmaDecoder::mmio_write(cpu::GuestAddress address, std::uint32_t width,
                            std::uint64_t raw_value) {
  if (address < kXmaMmioBase || address >= kXmaMmioBase + kXmaMmioSize ||
      (width != 1u && width != 2u && width != 4u)) {
    return;
  }
  const auto offset = static_cast<std::uint32_t>(address - kXmaMmioBase);
  const auto reg_offset = offset & ~3u;
  const auto byte_in_reg = offset & 3u;
  std::uint32_t value = static_cast<std::uint32_t>(raw_value);

  // Support narrow accesses through the same canonical big-endian register
  // value. Commands are dispatched from the resulting dword.
  if (width != 4u || byte_in_reg != 0) {
    std::lock_guard lock(mmio_mutex_);
    auto merged = mmio_registers_[reg_offset / 4u];
    if (width == 1u) {
      const auto shift = (3u - byte_in_reg) * 8u;
      merged = (merged & ~(0xFFu << shift)) | ((value & 0xFFu) << shift);
    } else if (width == 2u && byte_in_reg <= 2u) {
      const auto shift = (2u - byte_in_reg) * 8u;
      merged = (merged & ~(0xFFFFu << shift)) | ((value & 0xFFFFu) << shift);
    } else {
      return;
    }
    value = merged;
    mmio_registers_[reg_offset / 4u] = merged;
  } else {
    std::lock_guard lock(mmio_mutex_);
    // Xenon owns the physical context array. Preserve its address even if a
    // guest probes the hardware register with a write.
    if (reg_offset != kRegContextArrayAddress) {
      mmio_registers_[reg_offset / 4u] = value;
    }
    if (reg_offset == kRegNextContextIndex) {
      next_context_index_ = value % kXmaContextCount;
    }
  }

  const auto dispatch_group = [&](std::uint32_t base, auto&& fn) {
    if (reg_offset < base || reg_offset >= base + kContextRegisterGroupBytes) {
      return false;
    }
    const auto group = (reg_offset - base) / 4u;
    auto bits = value;
    for (std::uint32_t bit = 0; bit < 32u && bits; ++bit, bits >>= 1u) {
      if ((bits & 1u) == 0) continue;
      const auto index = group * 32u + bit;
      if (index >= kXmaContextCount || !allocated_[index].load()) continue;
      const auto context = context_guest_base_ +
          static_cast<cpu::GuestAddress>(index * kXmaContextBytes);
      fn(context);
    }
    return true;
  };

  if (dispatch_group(kRegContextKickBase,
                     [this](cpu::GuestAddress context) {
                       (void)enable_context(context);
                     })) {
    return;
  }
  if (dispatch_group(kRegContextLockBase,
                     [this](cpu::GuestAddress context) {
                       (void)disable_context(context, false);
                     })) {
    return;
  }
  (void)dispatch_group(kRegContextClearBase,
                       [this](cpu::GuestAddress context) {
                         (void)clear_context(context);
                       });
}

void XmaDecoder::worker_main() {
  while (worker_running_.load()) {
    bool did_work = false;
    for (std::size_t i = 0; i < kXmaContextCount; ++i) {
      if (!worker_running_.load()) break;
      if (allocated_[i].load() && runtimes_[i]) {
        bool enabled = false;
        {
          std::lock_guard lock(runtimes_[i]->mutex);
          enabled = runtimes_[i]->enabled;
        }
        if (enabled) did_work = decode_one(i) || did_work;
      }
    }
    if (!did_work) {
      std::unique_lock lock(worker_mutex_);
      // A plain timed wait lets enable_context() wake the worker immediately.
      // A predicate containing only shutdown state would swallow normal work
      // notifications and add an avoidable 20 ms decode latency.
      worker_cv_.wait_for(lock, std::chrono::milliseconds(20));
    }
  }
}

}  // namespace xenon::audio
