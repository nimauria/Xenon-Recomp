// XmaDecoder lifecycle, context allocation and the worker thread.

#include "audio/xma/xma_internal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace xenon::audio {

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
