// Decoding one XMA context: gathering frames from the input packets,
// FFmpeg XMAFRAMES decode and PCM output into the guest ring buffer.

#include "audio/xma/xma_internal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "xenon/logging/logger.hpp"
#include "xenon/logging/probe_log.hpp"

namespace xenon::audio {
namespace {

// Probe-trace timestamps: steady-clock milliseconds, comparable across the
// audio_trace_*.log files.
[[nodiscard]] std::int64_t trace_milliseconds() noexcept {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Stream speed per context, summarised once a second into
// audio_xma_rate_diag.log (it stays small enough to cover a whole session;
// the per-frame trace fills its cap within minutes):
//   decoded   - frames decoded, as samples per second against the stream's
//               own rate (1.00x is real time);
//   consumed  - how far the title's read offset moved through the output
//               ring, as the same ratio: the speed the title actually plays
//               the stream at. A voice consumed at 2x plays at double speed.
// A line marks each change of a context's stream format (rate, channel mode,
// output ring, subframes) as the title set it.
struct StreamStats {
  std::uint64_t frames{};
  std::uint64_t written_blocks{};
  std::uint64_t consumed_blocks{};
  int last_read{-1};
  std::uint32_t output{};
  std::uint8_t block_count{};
  std::uint8_t subframes{};
  std::uint8_t rate_id{0xFF};
  bool stereo{};
};
std::mutex g_stream_mutex;
std::int64_t g_stream_second = -1;
std::map<std::size_t, StreamStats> g_streams;

[[nodiscard]] int sample_rate_hz(std::uint8_t id) noexcept {
  constexpr int kRates[] = {24000, 32000, 44100, 48000};
  return id < 4u ? kRates[id] : 0;
}

// Called with g_stream_mutex held.
void flush_stream_second_locked(std::int64_t now) {
  if (now == g_stream_second) return;
  for (auto& [ctx, stats] : g_streams) {
    if (!stats.frames && !stats.consumed_blocks && !stats.written_blocks) continue;
    const auto rate = sample_rate_hz(stats.rate_id);
    // A 256-byte output block holds 128 samples of 16-bit mono or 64 of stereo.
    const double samples_per_block = stats.stereo ? 64.0 : 128.0;
    const double decoded = rate ? double(stats.frames) * 512.0 / rate : 0.0;
    const double consumed = rate ? double(stats.consumed_blocks) * samples_per_block / rate : 0.0;
    logging::append_probe_log(
        "audio_xma_rate_diag.log",
        "%llds ctx=%zu frames=%llu rate=%d ch=%d speed=%.2fx consumed=%.2fx written_blocks=%llu "
        "consumed_blocks=%llu\n",
        static_cast<long long>(g_stream_second), ctx,
        static_cast<unsigned long long>(stats.frames), rate, stats.stereo ? 2 : 1, decoded,
        consumed, static_cast<unsigned long long>(stats.written_blocks),
        static_cast<unsigned long long>(stats.consumed_blocks));
    stats.frames = stats.written_blocks = stats.consumed_blocks = 0;
  }
  g_stream_second = now;
}

// Each decode pass: the stream's format and the title's read offset.
void record_stream_pass(std::size_t context, const XmaContextData& data) {
  if (!logging::probe_logs_enabled()) return;
  std::scoped_lock lock(g_stream_mutex);
  flush_stream_second_locked(trace_milliseconds() / 1000);
  auto& stats = g_streams[context];
  if (stats.output != data.output_buffer_ptr || stats.block_count != data.output_buffer_block_count ||
      stats.rate_id != data.sample_rate || stats.stereo != data.is_stereo ||
      stats.subframes != data.subframe_decode_count) {
    logging::append_probe_log(
        "audio_xma_rate_diag.log",
        "%llds ctx=%zu stream: rate=%d (id %u) %s out=%08X blocks=%u subframes=%u padding=%u "
        "loop=%u in0=%08X/%u in1=%08X/%u\n",
        static_cast<long long>(trace_milliseconds() / 1000), context,
        sample_rate_hz(data.sample_rate), unsigned(data.sample_rate),
        data.is_stereo ? "stereo" : "mono", data.output_buffer_ptr,
        unsigned(data.output_buffer_block_count), unsigned(data.subframe_decode_count),
        unsigned(data.output_buffer_padding), unsigned(data.loop_count), data.input_buffer_0_ptr,
        unsigned(data.input_buffer_0_packet_count), data.input_buffer_1_ptr,
        unsigned(data.input_buffer_1_packet_count));
    stats.output = data.output_buffer_ptr;
    stats.block_count = data.output_buffer_block_count;
    stats.rate_id = data.sample_rate;
    stats.stereo = data.is_stereo;
    stats.subframes = data.subframe_decode_count;
    stats.last_read = -1;
  }
  const int read = data.output_buffer_read_offset;
  if (stats.last_read >= 0 && stats.block_count)
    stats.consumed_blocks +=
        static_cast<std::uint64_t>((read - stats.last_read + stats.block_count) % stats.block_count);
  stats.last_read = read;
}

void record_stream_frame(std::size_t context) {
  if (!logging::probe_logs_enabled()) return;
  std::scoped_lock lock(g_stream_mutex);
  flush_stream_second_locked(trace_milliseconds() / 1000);
  ++g_streams[context].frames;
}

void record_stream_written(std::size_t context, std::size_t blocks) {
  if (!logging::probe_logs_enabled()) return;
  std::scoped_lock lock(g_stream_mutex);
  flush_stream_second_locked(trace_milliseconds() / 1000);
  g_streams[context].written_blocks += blocks;
}

// Stream dumps (XENON_XMA_DUMP=<directory>): each context's streams as two
// files per stream, ctx<N>_s<serial>.xma - every input buffer the context
// decodes from, its raw 2048-byte packets in the order they were first used
// - and ctx<N>_s<serial>.pcm - the PCM written to the output ring, 16-bit
// big-endian as the title reads it - plus a line per stream in streams.txt
// (rate, channels, loop). tools/audio/xma_stream_frames.py walks the .xma
// straight through and counts the frames it holds, independently of this
// decoder's frame gathering, against the samples the .pcm holds.
// A new serial starts when the title changes the context's stream
// format or output ring. events.txt orders it all: a PASS line per decode
// pass with the context state the pass starts from (current buffer, read
// offset, valid flags, both input buffers, output ring offsets) and an
// APPEND line, with its packet position in the .xma, for each buffer added
// to a stream's .xma - enough to replay a stream's buffer hand-offs.
class StreamDump {
 public:
  static StreamDump& instance() {
    static StreamDump dump;
    return dump;
  }
  [[nodiscard]] bool enabled() const noexcept { return !directory_.empty(); }

  void pass(std::size_t context, const XmaContextData& data) {
    if (!enabled()) return;
    std::scoped_lock lock(mutex_);
    auto& stream = streams_[context];
    if (stream.serial == 0 || stream.output != data.output_buffer_ptr ||
        stream.rate_id != data.sample_rate || stream.stereo != data.is_stereo) {
      ++stream.serial;
      stream.output = data.output_buffer_ptr;
      stream.rate_id = data.sample_rate;
      stream.stereo = data.is_stereo;
      stream.buffer_hashes.clear();
      stream.xma_packets = 0;
      if (std::FILE* index = open("streams.txt", "ab")) {
        std::fprintf(index, "ctx%zu_s%u rate=%d channels=%d loop=%u out=%08X\n", context,
                     stream.serial, sample_rate_hz(data.sample_rate), data.is_stereo ? 2 : 1,
                     unsigned(data.loop_count), data.output_buffer_ptr);
        std::fclose(index);
      }
    }
    event("%lld ctx%zu_s%u PASS cur=%u rd=%u v0=%d v1=%d in0=%08X/%u in1=%08X/%u outr=%u "
          "outw=%u outv=%d loop=%u\n",
          static_cast<long long>(trace_milliseconds()), context, stream.serial,
          unsigned(data.current_buffer), unsigned(data.input_buffer_read_offset),
          data.input_buffer_0_valid ? 1 : 0, data.input_buffer_1_valid ? 1 : 0,
          data.input_buffer_0_ptr, unsigned(data.input_buffer_0_packet_count),
          data.input_buffer_1_ptr, unsigned(data.input_buffer_1_packet_count),
          unsigned(data.output_buffer_read_offset), unsigned(data.output_buffer_write_offset),
          data.output_buffer_valid ? 1 : 0, unsigned(data.loop_count));
  }

  // An input buffer decoded from: appended once per distinct content.
  void input(std::size_t context, std::uint32_t physical, std::span<const std::byte> bytes) {
    if (!enabled()) return;
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto b : bytes) {
      hash ^= std::to_integer<std::uint8_t>(b);
      hash *= 1099511628211ull;
    }
    std::scoped_lock lock(mutex_);
    auto& stream = streams_[context];
    auto& last = stream.buffer_hashes[physical];
    if (last == hash) return;
    last = hash;
    event("%lld ctx%zu_s%u APPEND phys=%08X packets=%zu at=%llu\n",
          static_cast<long long>(trace_milliseconds()), context, stream.serial, physical,
          bytes.size() / kXmaPacketBytes, static_cast<unsigned long long>(stream.xma_packets));
    stream.xma_packets += bytes.size() / kXmaPacketBytes;
    append(context, stream.serial, ".xma", bytes);
  }

  void output(std::size_t context, std::span<const std::byte> bytes) {
    if (!enabled()) return;
    std::scoped_lock lock(mutex_);
    append(context, streams_[context].serial, ".pcm", bytes);
  }

 private:
  StreamDump() {
    if (const char* value = std::getenv("XENON_XMA_DUMP"); value && *value) {
      directory_ = value;
      std::error_code ec;
      std::filesystem::create_directories(directory_, ec);
    }
  }
  // events.txt stays open for the run; each line is flushed so a run that
  // ends abruptly keeps what it logged. Called with mutex_ held.
  void event(const char* format, ...) {
    if (!events_) events_ = open("events.txt", "ab");
    if (!events_) return;
    va_list args;
    va_start(args, format);
    std::vfprintf(events_, format, args);
    va_end(args);
    std::fflush(events_);
  }
  std::FILE* open(const std::string& name, const char* mode) const {
    return std::fopen((directory_ / name).string().c_str(), mode);
  }
  void append(std::size_t context, unsigned serial, const char* extension,
              std::span<const std::byte> bytes) {
    const auto name = "ctx" + std::to_string(context) + "_s" + std::to_string(serial) + extension;
    if (std::FILE* file = open(name, "ab")) {
      std::fwrite(bytes.data(), 1, bytes.size(), file);
      std::fclose(file);
    }
  }
  struct Stream {
    unsigned serial{};
    std::uint32_t output{};
    std::uint8_t rate_id{0xFF};
    bool stereo{};
    std::map<std::uint32_t, std::uint64_t> buffer_hashes;
    std::uint64_t xma_packets{};
  };
  std::filesystem::path directory_;
  std::FILE* events_{};
  std::mutex mutex_;
  std::map<std::size_t, Stream> streams_;
};

constexpr std::uint32_t kBitsPerPacket = kXmaPacketBytes * 8u;
constexpr std::uint32_t kMaxFrameBits = 0x7FFFu;

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

// Copies `count` stream bits starting at `source_bit`, crossing packet
// boundaries the way the XMA engine does: past the end of a packet's data,
// the stream continues after the 32-bit header of the packet `skip_count + 1`
// packets on. Leaves `source_bit`/`packet_index` at the bit after the copy.
[[nodiscard]] XmaFrameGather copy_stream_bits(std::span<const std::byte> input,
                                              std::size_t& source_bit,
                                              std::size_t& packet_index,
                                              std::size_t count,
                                              std::vector<std::byte>& out) {
  std::size_t copied = 0;
  while (copied < count) {
    if ((packet_index + 1u) * kXmaPacketBytes > input.size()) {
      return XmaFrameGather::NeedsMoreInput;
    }
    const std::size_t packet_start = packet_index * kBitsPerPacket;
    const std::size_t packet_end = packet_start + kBitsPerPacket;
    if (source_bit < packet_start + 32u) source_bit = packet_start + 32u;
    const auto take = std::min<std::size_t>(count - copied, packet_end - source_bit);
    for (std::size_t i = 0; i < take; ++i) {
      std::uint8_t bit{};
      if (!read_bit(input, source_bit + i, bit)) return XmaFrameGather::NeedsMoreInput;
      append_bit(out, copied + i, bit);
    }
    copied += take;
    source_bit += take;
    if (copied == count) break;

    const auto header = parse_xma_packet_header(
        input.subspan(packet_index * kXmaPacketBytes, kXmaPacketBytes));
    packet_index += static_cast<std::size_t>(header.skip_count) + 1u;
    source_bit = packet_index * kBitsPerPacket + 32u;
  }
  return XmaFrameGather::Complete;
}

[[nodiscard]] std::int16_t float_to_s16(float value) noexcept {
  value = std::clamp(value, -1.0f, 1.0f);
  return static_cast<std::int16_t>(std::lrint(value * 32767.0f));
}

}  // namespace

std::size_t xma_frame_start_bit(std::span<const std::byte> input,
                                std::size_t read_offset_bits) {
  for (auto packet = read_offset_bits / kBitsPerPacket;
       (packet + 1u) * kXmaPacketBytes <= input.size(); ++packet) {
    const auto header = parse_xma_packet_header(
        input.subspan(packet * kXmaPacketBytes, kXmaPacketBytes));
    // No frame starts in this packet: it is all continuation.
    if (header.first_frame_offset_bits >= kBitsPerPacket) continue;
    const auto first_frame = packet * kBitsPerPacket + header.first_frame_offset_bits;
    const auto candidate = std::max(read_offset_bits, first_frame);
    if (candidate >= (packet + 1u) * kBitsPerPacket) continue;

    // A length of 0 or 0x7FFF is the padding after a packet's last frame,
    // not a frame: the stream goes on in its next packet. The previous
    // frame's "another frame follows" bit can still be set ahead of it.
    std::size_t source_bit = candidate;
    std::size_t length_packet = packet;
    std::vector<std::byte> length_bits;
    std::uint32_t length{};
    if (copy_stream_bits(input, source_bit, length_packet, 15u, length_bits) ==
            XmaFrameGather::Complete &&
        read_bits(length_bits, 0u, 15u, length) && (length == 0u || length == kMaxFrameBits)) {
      packet += header.skip_count;
      read_offset_bits = (packet + 1u) * kBitsPerPacket;
      continue;
    }
    return candidate;
  }
  return input.size() * 8u;
}

XmaGatheredFrame gather_xma_frame(std::span<const std::byte> input, std::size_t start_bit) {
  XmaGatheredFrame result{};
  if (input.size() < kXmaPacketBytes || start_bit >= input.size() * 8u) {
    result.status = XmaFrameGather::NeedsMoreInput;
    return result;
  }

  // The 15-bit frame length may itself straddle two packets.
  std::size_t source_bit = start_bit;
  std::size_t packet_index = source_bit / kBitsPerPacket;
  std::vector<std::byte> length_bits;
  result.status = copy_stream_bits(input, source_bit, packet_index, 15u, length_bits);
  if (result.status != XmaFrameGather::Complete) return result;
  std::uint32_t frame_bits{};
  if (!read_bits(length_bits, 0u, 15u, frame_bits) || frame_bits < 16u ||
      frame_bits >= kMaxFrameBits) {
    result.status = XmaFrameGather::Invalid;
    return result;
  }

  result.frame_bits = frame_bits;
  result.packed_bits.resize((frame_bits + 7u) / 8u, std::byte{0});
  source_bit = start_bit;
  packet_index = source_bit / kBitsPerPacket;
  result.status = copy_stream_bits(input, source_bit, packet_index, frame_bits,
                                   result.packed_bits);
  if (result.status != XmaFrameGather::Complete) return result;

  result.final_packet = packet_index;
  result.final_source_bit = source_bit;
  std::uint8_t follows{};
  if (read_bit(result.packed_bits, frame_bits - 1u, follows)) {
    result.follows_in_packet = follows != 0;
  }
  return result;
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
  {
    PassObserver observer;
    {
      std::lock_guard lock(pass_observer_mutex_);
      observer = pass_observer_;
    }
    if (observer) observer(context);
  }
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
  decode_passes_.fetch_add(1, std::memory_order_relaxed);
  record_stream_pass(index, data);
  StreamDump::instance().pass(index, data);

  if (!data.output_buffer_valid || !data.output_buffer_ptr ||
      !data.output_buffer_block_count) {
    idle_passes_[kIdleNoOutputBuffer].fetch_add(1, std::memory_order_relaxed);
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
      idle_passes_[kIdleOutputFull].fetch_add(1, std::memory_order_relaxed);
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

    StreamDump::instance().output(index, source);
    logging::append_probe_log(
        "audio_trace_xma.log",
        "%lld write ctx=%zu out=%08X blocks=%u read=%u write=%zu->%zu bytes=%zu valid_in=%d%d\n",
        static_cast<long long>(trace_milliseconds()), index, data.output_buffer_ptr,
        data.output_buffer_block_count, data.output_buffer_read_offset,
        write_offset / kXmaOutputBlockBytes,
        ((write_offset + bytes_to_write) % capacity) / kXmaOutputBlockBytes, bytes_to_write,
        data.input_buffer_0_valid ? 1 : 0, data.input_buffer_1_valid ? 1 : 0);
    record_stream_written(index, blocks_to_write);
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
    idle_passes_[kIdleOutputFull].fetch_add(1, std::memory_order_relaxed);
    finish();
    return false;
  }

  if (!data.input_buffer_0_valid && !data.input_buffer_1_valid) {
    idle_passes_[kIdleNoInput].fetch_add(1, std::memory_order_relaxed);
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
  StreamDump::instance().input(index, input_physical, input);

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
  // Regression (AC6 audio dropouts): after an input-buffer swap the read
  // offset is bit 32, the start of the first packet's data, which is the tail
  // of a frame begun in the previous buffer. Decoding from there read part of
  // that tail as a frame length (a parser error) or handed it to the decoder
  // (a decode error), and either error stopped the voice.
  std::size_t start_bit = xma_frame_start_bit(
      input, std::max<std::uint32_t>(32u, data.input_buffer_read_offset));
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

  const auto gathered_frame = gather_xma_frame(input, start_bit);
  if (gathered_frame.status == XmaFrameGather::NeedsMoreInput &&
      input.size() == current_input_bytes) {
    // The frame continues into the other input buffer, which the title has
    // not made valid yet. Consume nothing and wait for it, as the engine
    // does; the title's next kick resumes here with the whole frame.
    idle_passes_[kIdleNoInput].fetch_add(1, std::memory_order_relaxed);
    finish();
    return false;
  }
  if (gathered_frame.status != XmaFrameGather::Complete) {
    // The stream state that led here, for the run's audio.log (the decoder
    // records the error itself when it is published).
    char detail[224];
    std::snprintf(detail, sizeof(detail),
                  "XMA context 0x%08X cannot parse a frame: %s at bit %zu (read offset %u), "
                  "buffer %u of 0:%s%u/1:%s%u packets, %zu input bytes",
                  static_cast<unsigned>(context),
                  gathered_frame.status == XmaFrameGather::Invalid ? "impossible length"
                                                                   : "frame runs past both buffers",
                  start_bit, static_cast<unsigned>(data.input_buffer_read_offset),
                  current ? 1u : 0u, data.input_buffer_0_valid ? "" : "invalid ",
                  static_cast<unsigned>(data.input_buffer_0_packet_count),
                  data.input_buffer_1_valid ? "" : "invalid ",
                  static_cast<unsigned>(data.input_buffer_1_packet_count), input.size());
    logging::Logger::instance().record(logging::Level::Warning, "audio", detail);
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
  const auto payload_bytes = (gathered_frame.frame_bits + 7u) / 8u;
  if (av_new_packet(runtime.packet, static_cast<int>(payload_bytes + 1u)) < 0) {
    data.error_status = 7;
    data.error_set = true;
    (void)merge_hardware_progress(context, initial, data);
    finish();
    return false;
  }
  const auto end_padding =
      static_cast<std::uint8_t>((8u - (gathered_frame.frame_bits & 7u)) & 7u);
  runtime.packet->data[0] = static_cast<std::uint8_t>(end_padding << 2u);
  std::memcpy(runtime.packet->data + 1, gathered_frame.packed_bits.data(), payload_bytes);

  av_frame_unref(runtime.frame);
  const int send_result = avcodec_send_packet(runtime.codec_context, runtime.packet);
  const int receive_result =
      send_result < 0 ? send_result
                      : avcodec_receive_frame(runtime.codec_context, runtime.frame);
  // A frame the decoder rejects still occupies one frame of the stream's
  // timeline. Stopping the voice, or dropping the frame, puts it behind the
  // other streams of a multi-stream voice for good - AC6 plays its cutscene
  // 5.1 as parallel stereo streams, and a stream a frame out of step combs
  // against the rest (a "robotic" echo). As the reference decoder does, the
  // frame becomes silence and the stream moves past it.
  const bool frame_decoded =
      receive_result >= 0 &&
      runtime.frame->nb_samples >= static_cast<int>(kXmaSamplesPerFrame) &&
      runtime.frame->format == AV_SAMPLE_FMT_FLTP;
  if (!frame_decoded) {
    decode_errors_[8].fetch_add(1, std::memory_order_relaxed);
    char entry[160];
    std::snprintf(entry, sizeof(entry),
                  "XMA context 0x%08X: the decoder rejected the %u-bit frame at bit %zu; "
                  "playing it as silence to keep the stream in step",
                  static_cast<unsigned>(context), gathered_frame.frame_bits, start_bit);
    logging::Logger::instance().record(logging::Level::Warning, "audio", entry);
  }

  const auto frame_bytes = static_cast<std::size_t>(kXmaSamplesPerFrame) *
                           channels * sizeof(std::int16_t);
  std::array<std::byte, Runtime::kMaxFrameBytes> decoded_pcm{};
  std::size_t decoded_offset = 0;
  float frame_peak = 0.0f;
  for (std::uint32_t sample = 0; frame_decoded && sample < kXmaSamplesPerFrame; ++sample) {
    for (int ch = 0; ch < channels; ++ch) {
      const auto* plane =
          reinterpret_cast<const float*>(runtime.frame->extended_data[ch]);
      frame_peak = std::max(frame_peak, std::abs(plane[sample]));
      const auto value = static_cast<std::uint16_t>(float_to_s16(plane[sample]));
      decoded_pcm[decoded_offset++] = std::byte((value >> 8u) & 0xFFu);
      decoded_pcm[decoded_offset++] = std::byte(value & 0xFFu);
    }
  }
  if (frame_decoded) {
    frames_decoded_.fetch_add(1, std::memory_order_relaxed);
    record_stream_frame(index);
  }
  logging::append_probe_log(
      "audio_trace_xma.log", "%lld frame ctx=%zu in=%08X bit=%zu rate=%d ch=%d peak=%.5f loop=%u\n",
      static_cast<long long>(trace_milliseconds()), index, input_physical, start_bit, sample_rate,
      channels, static_cast<double>(frame_peak), data.loop_count);
  for (auto bits = peak_bits_.load(std::memory_order_relaxed);
       std::bit_cast<float>(bits) < frame_peak &&
       !peak_bits_.compare_exchange_weak(bits, std::bit_cast<std::uint32_t>(frame_peak),
                                         std::memory_order_relaxed);) {
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
  if (gathered_frame.follows_in_packet &&
      gathered_frame.final_source_bit < (gathered_frame.final_packet + 1u) * kBitsPerPacket) {
    next_bit = gathered_frame.final_source_bit;
  } else if (gathered_frame.final_packet != start_packet &&
             gathered_frame.final_source_bit <
                 (gathered_frame.final_packet + 1u) * kBitsPerPacket) {
    // Regression (AC6 briefing voice at double speed, choppy, "Garuda team"
    // heard as "green"; music skipping): the last frame to start in a packet
    // ran on into the stream's next packet, and the next frame starts in
    // that packet, straight after the run-on (at its header's first-frame
    // offset). Taking "the packet after the one the frame ended in" skipped
    // every frame of that packet - half of each voice line, whose frames
    // run on across every packet. xenia and rexglue step from the packet the
    // frame started in (packet_index + skip + 1), which is this packet.
    next_bit = xma_frame_start_bit(input, gathered_frame.final_source_bit);
  } else {
    const auto final_packet_span = std::span<const std::byte>(input).subspan(
        gathered_frame.final_packet * kXmaPacketBytes, kXmaPacketBytes);
    const auto final_header = parse_xma_packet_header(final_packet_span);
    const auto next_packet = gathered_frame.final_packet +
                             static_cast<std::size_t>(final_header.skip_count) + 1u;
    next_bit = xma_frame_start_bit(input, next_packet * kBitsPerPacket);
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

}  // namespace xenon::audio
