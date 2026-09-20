#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>

#include "xenon/audio/backend.hpp"
#include "xenon/audio/mixer.hpp"
#include "xenon/audio/xma.hpp"
#include "xenon/cpu/types.hpp"
#include "xenon/memory/address_space.hpp"

namespace xenon::audio {

class AudioSystem final {
 public:
  using GuestCallbackInvoker =
      std::function<bool(cpu::GuestAddress callback, cpu::GuestAddress argument)>;

  explicit AudioSystem(memory::AddressSpace& memory,
                       std::unique_ptr<AudioBackend> backend = {});
  ~AudioSystem();

  AudioSystem(const AudioSystem&) = delete;
  AudioSystem& operator=(const AudioSystem&) = delete;

  [[nodiscard]] bool initialize(std::string* error = nullptr);
  void shutdown() noexcept;
  [[nodiscard]] bool initialized() const noexcept { return initialized_.load(); }

  [[nodiscard]] Mixer& mixer() noexcept { return mixer_; }
  [[nodiscard]] XmaDecoder& xma() noexcept { return xma_; }
  [[nodiscard]] const XmaDecoder& xma() const noexcept { return xma_; }

  void set_guest_callback_invoker(GuestCallbackInvoker invoker);

  [[nodiscard]] std::optional<RenderClientHandle> register_render_client(
      cpu::GuestAddress callback, std::uint32_t callback_argument);
  [[nodiscard]] bool unregister_render_client(RenderClientHandle handle);
  [[nodiscard]] bool submit_render_frame(RenderClientHandle handle,
                                         cpu::GuestAddress samples);
  [[nodiscard]] bool suspend_render_clients(bool suspend);
  [[nodiscard]] std::uint32_t render_driver_tic() const noexcept {
    return static_cast<std::uint32_t>(render_tic_samples_.load());
  }
  [[nodiscard]] std::uint32_t underrun_count() const noexcept {
    return underrun_count_.load();
  }

  [[nodiscard]] std::uint32_t speaker_config() const noexcept {
    return speaker_config_.load();
  }
  void set_speaker_config(std::uint32_t value) noexcept {
    speaker_config_.store(value);
  }
  [[nodiscard]] std::uint32_t voice_category_change_mask() noexcept;
  [[nodiscard]] float voice_category_volume(std::uint32_t category) const noexcept;
  [[nodiscard]] bool set_voice_category_volume(std::uint32_t category, float value) noexcept;

  void enable_ducker(bool enabled) noexcept;
  [[nodiscard]] bool ducker_enabled() const noexcept { return ducker_enabled_.load(); }
  void set_ducker_level(float value) noexcept;
  void set_ducker_threshold(float value) noexcept;
  void set_ducker_attack(float seconds) noexcept;
  void set_ducker_release(float seconds) noexcept;
  void set_ducker_hold(float seconds) noexcept;
  [[nodiscard]] float ducker_level() const noexcept { return ducker_level_.load(); }
  [[nodiscard]] float ducker_threshold() const noexcept {
    return ducker_threshold_.load();
  }
  [[nodiscard]] float ducker_attack() const noexcept { return ducker_attack_.load(); }
  [[nodiscard]] float ducker_release() const noexcept { return ducker_release_.load(); }
  [[nodiscard]] float ducker_hold() const noexcept { return ducker_hold_.load(); }

  // Exposed for deterministic tests/fake backends.
  void render(std::span<float> interleaved_stereo);

 private:
  struct RenderFrame {
    std::array<float, kRenderFrameSamples * kHostChannels> stereo{};
  };

  struct RenderClient {
    bool allocated{};
    // A retiring client remains reserved until every callback already handed
    // to the guest has returned. This keeps the wrapped callback argument
    // alive without holding clients_mutex_ across guest execution.
    bool retiring{};
    bool suspended{};
    cpu::GuestAddress callback{};
    cpu::GuestAddress callback_argument_wrapper{};
    std::deque<RenderFrame> queue{};
    std::size_t frame_offset{};
    // Callback credits mirror the Xbox render driver's bounded semaphore:
    // one callback may be issued per free frame slot, and a credit is returned
    // only when a submitted 256-sample frame is actually consumed.
    std::size_t callback_credits{};
    std::size_t callback_in_flight{};
  };

  [[nodiscard]] bool translate_render_frame(cpu::GuestAddress samples,
                                            RenderFrame& out);
  [[nodiscard]] static bool valid_client_handle(RenderClientHandle handle,
                                                std::size_t& index) noexcept;
  void callback_pump();
  void update_ducker(std::span<float> samples);

  memory::AddressSpace& memory_;
  std::unique_ptr<AudioBackend> backend_{};
  Mixer mixer_{};
  XmaDecoder xma_;
  std::atomic_bool initialized_{false};

  mutable std::mutex clients_mutex_{};
  std::array<RenderClient, kMaxRenderClients> clients_{};
  std::atomic_bool callback_running_{false};
  std::condition_variable callback_cv_{};
  std::thread callback_thread_{};
  GuestCallbackInvoker guest_callback_invoker_{};
  std::atomic<std::uint32_t> underrun_count_{0};
  // Xbox render-driver clock, in 48 kHz samples consumed by the host. This
  // advances through underrun silence as well as submitted audio so guest AV
  // synchronization follows real playback time rather than queue occupancy.
  std::atomic<std::uint64_t> render_tic_samples_{0};

  std::atomic<std::uint32_t> speaker_config_{0x00010001u};
  std::array<std::atomic<float>, 32> category_volumes_{};
  std::atomic<std::uint32_t> category_change_mask_{0};

  std::atomic_bool ducker_enabled_{false};
  std::atomic<float> ducker_level_{0.5f};
  std::atomic<float> ducker_threshold_{0.25f};
  std::atomic<float> ducker_attack_{0.01f};
  std::atomic<float> ducker_release_{0.25f};
  std::atomic<float> ducker_hold_{0.05f};
  float ducker_gain_{1.0f};
  std::uint32_t ducker_hold_frames_{};
};

}  // namespace xenon::audio
