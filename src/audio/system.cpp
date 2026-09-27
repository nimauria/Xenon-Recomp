#include "xenon/audio/system.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "xenon/memory/types.hpp"

namespace xenon::audio {
namespace {

[[nodiscard]] float load_be_float(const std::byte* p) noexcept {
  const std::uint32_t bits =
      (std::uint32_t(std::to_integer<std::uint8_t>(p[0])) << 24u) |
      (std::uint32_t(std::to_integer<std::uint8_t>(p[1])) << 16u) |
      (std::uint32_t(std::to_integer<std::uint8_t>(p[2])) << 8u) |
      std::uint32_t(std::to_integer<std::uint8_t>(p[3]));
  return std::bit_cast<float>(bits);
}

}  // namespace

AudioSystem::AudioSystem(memory::AddressSpace& memory,
                         std::unique_ptr<AudioBackend> backend)
    : memory_(memory),
      backend_(std::move(backend)),
      xma_(memory) {
  for (auto& volume : category_volumes_) volume.store(1.0f);
}

AudioSystem::~AudioSystem() { shutdown(); }

bool AudioSystem::initialize(std::string* error, bool auto_start_callback_pump) {
  if (initialized_.load()) return true;
  if (!backend_) backend_ = create_sdl_audio_backend();
  if (!backend_) {
    if (error) *error = "no production audio backend is available";
    return false;
  }

  std::string xma_error;
  if (!xma_.initialize(&xma_error)) {
    if (error) *error = "XMA initialization failed: " + xma_error;
    return false;
  }

  AudioBackendConfig config{};
  config.sample_rate = kXboxSampleRate;
  config.channels = kHostChannels;
  config.callback_frames = kRenderFrameSamples;
  std::string backend_error;
  if (!backend_->open(config,
                      [this](std::span<float> output) { render(output); },
                      &backend_error)) {
    xma_.shutdown();
    if (error) *error = "host audio backend failed: " + backend_error;
    return false;
  }

  if (auto_start_callback_pump) {
    callback_running_.store(true);
    callback_thread_ = std::thread(&AudioSystem::callback_pump, this);
  }
  initialized_.store(true);
  return true;
}

void AudioSystem::stop_guest_callback_pump() noexcept {
  // Stop guest callbacks before tearing down the native device or freeing any
  // wrapped callback arguments. A callback is allowed to submit audio, so it
  // must never race client destruction during shutdown.
  callback_running_.store(false);
  callback_cv_.notify_all();
  if (callback_thread_.joinable()) callback_thread_.join();
}

void AudioSystem::shutdown() noexcept {
  if (!initialized_.exchange(false) && !xma_.initialized()) return;

  stop_guest_callback_pump();

  if (backend_) backend_->close();

  {
    std::lock_guard lock(clients_mutex_);
    for (auto& client : clients_) {
      if (client.callback_argument_wrapper) {
        (void)memory_.release(client.callback_argument_wrapper);
      }
      client = {};
    }
    guest_callback_invoker_ = {};
  }
  xma_.shutdown();
}

void AudioSystem::set_guest_callback_invoker(GuestCallbackInvoker invoker) {
  std::lock_guard lock(clients_mutex_);
  guest_callback_invoker_ = std::move(invoker);
}

std::optional<RenderClientHandle> AudioSystem::register_render_client(
    cpu::GuestAddress callback, std::uint32_t callback_argument) {
  if (!initialized_.load()) return std::nullopt;
  cpu::GuestAddress wrapper{};
  if (!memory_.allocate(sizeof(std::uint32_t), alignof(std::uint32_t),
                        memory::kReadWrite, false, wrapper)) {
    return std::nullopt;
  }
  memory_.write32_be(wrapper, callback_argument);

  std::lock_guard lock(clients_mutex_);
  for (std::size_t i = 0; i < clients_.size(); ++i) {
    if (!clients_[i].allocated && !clients_[i].retiring) {
      clients_[i].allocated = true;
      clients_[i].callback = callback;
      clients_[i].callback_argument_wrapper = wrapper;
      clients_[i].frame_offset = 0;
      clients_[i].callback_credits = kMaxQueuedRenderFrames;
      callback_cv_.notify_all();
      return 0x41550000u | static_cast<RenderClientHandle>(i);
    }
  }
  (void)memory_.release(wrapper);
  return std::nullopt;
}

bool AudioSystem::valid_client_handle(RenderClientHandle handle,
                                      std::size_t& index) noexcept {
  if ((handle & 0xFFFF0000u) != 0x41550000u) return false;
  index = handle & 0xFFFFu;
  return index < kMaxRenderClients;
}

bool AudioSystem::unregister_render_client(RenderClientHandle handle) {
  std::size_t index{};
  if (!valid_client_handle(handle, index)) return false;

  cpu::GuestAddress wrapper{};
  std::unique_lock lock(clients_mutex_);
  auto& client = clients_[index];
  if (!client.allocated || client.retiring) return false;

  client.allocated = false;
  client.retiring = true;
  client.suspended = true;
  client.callback_credits = 0;
  client.queue.clear();
  client.frame_offset = 0;

  if (client.callback_in_flight == 0) {
    wrapper = client.callback_argument_wrapper;
    client = {};
    lock.unlock();
    if (wrapper) (void)memory_.release(wrapper);
    return true;
  }

  // A guest callback may unregister itself. Waiting in that case would wait
  // for the currently executing callback to return to this very thread. Leave
  // retirement to callback_pump() after the invoker unwinds.
  if (std::this_thread::get_id() == callback_thread_.get_id()) return true;

  callback_cv_.wait(lock, [&] { return !clients_[index].retiring; });
  return true;
}

bool AudioSystem::translate_render_frame(cpu::GuestAddress samples,
                                         RenderFrame& out) {
  if (!samples) return false;
  constexpr std::size_t kInputFloats =
      static_cast<std::size_t>(kXboxRenderChannels) * kRenderFrameSamples;
  std::array<std::byte, kInputFloats * sizeof(float)> bytes{};
  memory_.read_bytes(samples, bytes);

  for (std::size_t sample = 0; sample < kRenderFrameSamples; ++sample) {
    const auto read_channel = [&](std::size_t channel) {
      const auto offset =
          (channel * kRenderFrameSamples + sample) * sizeof(float);
      return load_be_float(bytes.data() + offset);
    };
    const float fl = read_channel(0);
    const float fr = read_channel(1);
    const float center = read_channel(2);
    const float back_left = read_channel(4);
    const float back_right = read_channel(5);
    // Xbox default 5.1 order: FL, FR, C, LFE, BL, BR. LFE is intentionally
    // omitted from stereo downmix; scaling keeps full-scale multichannel
    // content from clipping under normal conditions.
    out.stereo[sample * 2] =
        (fl + back_left + center * 0.5f) * (1.0f / 2.5f);
    out.stereo[sample * 2 + 1] =
        (fr + back_right + center * 0.5f) * (1.0f / 2.5f);
  }
  return true;
}

bool AudioSystem::submit_render_frame(RenderClientHandle handle,
                                      cpu::GuestAddress samples) {
  std::size_t index{};
  if (!valid_client_handle(handle, index)) return false;
  RenderFrame frame{};
  if (!translate_render_frame(samples, frame)) return false;

  std::lock_guard lock(clients_mutex_);
  auto& client = clients_[index];
  if (!client.allocated || client.suspended ||
      client.queue.size() >= kMaxQueuedRenderFrames) {
    return false;
  }
  client.queue.push_back(std::move(frame));
  return true;
}

bool AudioSystem::suspend_render_clients(bool suspend) {
  std::lock_guard lock(clients_mutex_);
  bool any = false;
  for (auto& client : clients_) {
    if (!client.allocated) continue;
    client.suspended = suspend;
    any = true;
  }
  if (!suspend) callback_cv_.notify_all();
  return any;
}

std::uint32_t AudioSystem::voice_category_change_mask() noexcept {
  return category_change_mask_.exchange(0);
}

float AudioSystem::voice_category_volume(std::uint32_t category) const noexcept {
  return category_volumes_[category & 31u].load();
}

bool AudioSystem::set_voice_category_volume(std::uint32_t category,
                                            float value) noexcept {
  if (!std::isfinite(value) || value < 0.0f || value > 16.0f) return false;
  const auto index = category & 31u;
  const auto old = category_volumes_[index].exchange(value);
  if (old != value) category_change_mask_.fetch_or(1u << index);
  return true;
}

void AudioSystem::enable_ducker(bool enabled) noexcept {
  // ducker_gain_ and ducker_hold_frames_ are render-thread-owned state. Only
  // publish the control bit here; update_ducker() performs the state reset on
  // the audio callback thread to avoid a host-thread data race.
  ducker_enabled_.store(enabled);
}

void AudioSystem::set_ducker_level(float value) noexcept {
  if (std::isfinite(value)) ducker_level_.store(std::clamp(value, 0.0f, 1.0f));
}
void AudioSystem::set_ducker_threshold(float value) noexcept {
  if (std::isfinite(value)) ducker_threshold_.store(std::clamp(value, 0.0f, 1.0f));
}
void AudioSystem::set_ducker_attack(float seconds) noexcept {
  if (std::isfinite(seconds)) ducker_attack_.store(std::max(seconds, 0.0001f));
}
void AudioSystem::set_ducker_release(float seconds) noexcept {
  if (std::isfinite(seconds)) ducker_release_.store(std::max(seconds, 0.0001f));
}
void AudioSystem::set_ducker_hold(float seconds) noexcept {
  if (std::isfinite(seconds)) ducker_hold_.store(std::max(seconds, 0.0f));
}

void AudioSystem::set_master_volume(float value) noexcept {
  if (std::isfinite(value)) master_volume_.store(std::clamp(value, 0.0f, 1.0f));
}

void AudioSystem::update_ducker(std::span<float> samples) {
  if (samples.empty()) return;
  if (!ducker_enabled_.load()) {
    ducker_gain_ = 1.0f;
    ducker_hold_frames_ = 0;
    return;
  }
  float peak = 0.0f;
  for (float sample : samples) peak = std::max(peak, std::abs(sample));

  const auto frames = static_cast<std::uint32_t>(samples.size() / 2u);
  float target = 1.0f;
  if (peak >= ducker_threshold_.load()) {
    target = ducker_level_.load();
    ducker_hold_frames_ = static_cast<std::uint32_t>(
        ducker_hold_.load() * static_cast<float>(kXboxSampleRate));
  } else if (ducker_hold_frames_) {
    target = ducker_level_.load();
    ducker_hold_frames_ = frames >= ducker_hold_frames_ ? 0u
                                                        : ducker_hold_frames_ - frames;
  }

  const float seconds = target < ducker_gain_ ? ducker_attack_.load()
                                              : ducker_release_.load();
  const float blend = std::clamp(
      static_cast<float>(frames) / (seconds * static_cast<float>(kXboxSampleRate)),
      0.0f, 1.0f);
  ducker_gain_ += (target - ducker_gain_) * blend;
  for (float& sample : samples) sample *= ducker_gain_;
}

void AudioSystem::render(std::span<float> interleaved_stereo) {
  if (interleaved_stereo.empty()) return;
  std::fill(interleaved_stereo.begin(), interleaved_stereo.end(), 0.0f);
  const auto output_frames = interleaved_stereo.size() / 2u;
  bool had_render_driver_audio = false;
  bool had_active_render_client = false;

  {
    std::lock_guard lock(clients_mutex_);
    for (auto& client : clients_) {
      if (!client.allocated || client.suspended) continue;
      had_active_render_client = true;
      std::size_t output_frame = 0;
      while (output_frame < output_frames && !client.queue.empty()) {
        auto& frame = client.queue.front();
        const auto available = kRenderFrameSamples - client.frame_offset;
        const auto take = std::min<std::size_t>(available,
                                                output_frames - output_frame);
        for (std::size_t i = 0; i < take; ++i) {
          const auto src = (client.frame_offset + i) * 2u;
          const auto dst = (output_frame + i) * 2u;
          interleaved_stereo[dst] += frame.stereo[src];
          interleaved_stereo[dst + 1u] += frame.stereo[src + 1u];
        }
        had_render_driver_audio = true;
        output_frame += take;
        client.frame_offset += take;
        if (client.frame_offset == kRenderFrameSamples) {
          client.queue.pop_front();
          client.frame_offset = 0;
          if (client.callback_credits < kMaxQueuedRenderFrames) {
            ++client.callback_credits;
          }
          callback_cv_.notify_all();
        }
      }
    }
  }

  if (had_active_render_client && !had_render_driver_audio) {
    underrun_count_.fetch_add(1);
  }

  // Generic voices and the Xbox render-driver path share the same native
  // output. Mixing is additive so neither path silently suppresses the other.
  mixer_.mix(interleaved_stereo, kXboxSampleRate);
  update_ducker(interleaved_stereo);
  const float host_gain = muted_.load() ? 0.0f : master_volume_.load();
  for (float& sample : interleaved_stereo) {
    sample = std::clamp(sample * host_gain, -1.0f, 1.0f);
  }

  // XAudioGetRenderDriverTic is a consumed-sample clock, not a count of
  // successfully submitted buffers. Advance even when we produced silence.
  render_tic_samples_.fetch_add(output_frames, std::memory_order_relaxed);
}

void AudioSystem::callback_pump() {
  while (callback_running_.load()) {
    struct Pending {
      std::size_t index{};
      cpu::GuestAddress callback{};
      cpu::GuestAddress wrapper{};
    };
    std::vector<Pending> pending;
    GuestCallbackInvoker invoker;
    {
      std::unique_lock lock(clients_mutex_);
      callback_cv_.wait_for(lock, std::chrono::milliseconds(2));
      if (!callback_running_.load()) break;
      invoker = guest_callback_invoker_;
      if (invoker) {
        for (std::size_t i = 0; i < clients_.size(); ++i) {
          auto& client = clients_[i];
          if (!client.allocated || client.retiring || client.suspended ||
              !client.callback ||
              client.queue.size() >= kMaxQueuedRenderFrames ||
              client.callback_credits == 0) {
            continue;
          }
          --client.callback_credits;
          ++client.callback_in_flight;
          pending.push_back(
              {i, client.callback, client.callback_argument_wrapper});
        }
      }
    }

    for (const auto& item : pending) {
      if (callback_running_.load()) {
        (void)invoker(item.callback, item.wrapper);
      }

      cpu::GuestAddress retired_wrapper{};
      {
        std::lock_guard lock(clients_mutex_);
        auto& client = clients_[item.index];
        if (client.callback_in_flight) --client.callback_in_flight;
        if (client.retiring && client.callback_in_flight == 0) {
          retired_wrapper = client.callback_argument_wrapper;
          client = {};
        }
        callback_cv_.notify_all();
      }
      if (retired_wrapper) (void)memory_.release(retired_wrapper);
    }
  }
}

}  // namespace xenon::audio
