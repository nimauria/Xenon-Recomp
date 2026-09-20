#include <array>
#include <cassert>
#include <atomic>
#include <bit>
#include <chrono>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "xenon/audio/system.hpp"
#include "xenon/memory/types.hpp"

using namespace xenon;
using namespace xenon::audio;

namespace {

class FakeBackend final : public AudioBackend {
 public:
  bool open(const AudioBackendConfig& config, HostRenderCallback callback,
            std::string*) override {
    config_ = config;
    callback_ = std::move(callback);
    open_ = true;
    return true;
  }
  void close() noexcept override { open_ = false; }
  bool is_open() const noexcept override { return open_; }
  void pump(std::span<float> samples) { callback_(samples); }
  AudioBackendConfig config_{};
  HostRenderCallback callback_{};
  bool open_{};
};

void write_be_float(memory::AddressSpace& memory, cpu::GuestAddress address,
                    float value) {
  memory.write32_be(address, std::bit_cast<std::uint32_t>(value));
}

}  // namespace

int main() {
  memory::AddressSpace memory(memory::GuestTranslationMode::Compact);
  assert(memory.initialize());
  auto backend = std::make_unique<FakeBackend>();
  auto* fake = backend.get();
  AudioSystem audio(memory, std::move(backend));
  std::string error;
  assert(audio.initialize(&error));
  assert(fake->is_open());
  assert(fake->config_.sample_rate == 48000);
  assert(fake->config_.channels == 2);

  // Render callbacks are credit/semaphore driven, not periodic polling. A
  // callback that submits nothing can consume at most the bounded initial
  // credits and must not be called forever every few milliseconds.
  std::atomic<std::uint32_t> callback_count{0};
  audio.set_guest_callback_invoker(
      [&](cpu::GuestAddress callback, cpu::GuestAddress argument) {
        assert(callback == 0x1000u);
        assert(argument != 0);
        callback_count.fetch_add(1);
        return true;
      });
  const auto credit_client = audio.register_render_client(0x1000u, 0xCAFEBABEu);
  assert(credit_client);
  for (int i = 0; i < 100 && callback_count.load() < kMaxQueuedRenderFrames; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  assert(callback_count.load() == kMaxQueuedRenderFrames);
  const auto saturated_callbacks = callback_count.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  assert(callback_count.load() == saturated_callbacks);
  assert(audio.unregister_render_client(*credit_client));

  // Unregistering from another thread must not free the wrapped callback
  // argument while a guest callback is still using it. This regresses the
  // callback/unregister lifetime race.
  std::atomic_bool callback_entered{false};
  std::atomic_bool allow_callback_return{false};
  std::atomic_bool unregister_finished{false};
  audio.set_guest_callback_invoker(
      [&](cpu::GuestAddress callback, cpu::GuestAddress argument) {
        assert(callback == 0x2000u);
        assert(memory.read32_be(argument) == 0x13572468u);
        callback_entered.store(true);
        while (!allow_callback_return.load()) {
          std::this_thread::yield();
        }
        // The wrapper must remain valid through the last instruction of the
        // callback even after unregister has begun on another thread.
        assert(memory.read32_be(argument) == 0x13572468u);
        return true;
      });
  const auto lifetime_client =
      audio.register_render_client(0x2000u, 0x13572468u);
  assert(lifetime_client);
  for (int i = 0; i < 100 && !callback_entered.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  assert(callback_entered.load());
  std::thread unregister_thread([&] {
    assert(audio.unregister_render_client(*lifetime_client));
    unregister_finished.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  assert(!unregister_finished.load());
  allow_callback_return.store(true);
  unregister_thread.join();
  assert(unregister_finished.load());

  cpu::GuestAddress frame_address{};
  constexpr std::size_t kInputFloats = kXboxRenderChannels * kRenderFrameSamples;
  assert(memory.allocate(static_cast<std::uint32_t>(kInputFloats * sizeof(float)),
                         16, memory::kReadWrite, false, frame_address));
  for (std::size_t i = 0; i < kRenderFrameSamples; ++i) {
    write_be_float(memory, frame_address + static_cast<std::uint32_t>((0 * kRenderFrameSamples + i) * 4), 1.0f);
    write_be_float(memory, frame_address + static_cast<std::uint32_t>((1 * kRenderFrameSamples + i) * 4), 0.5f);
    write_be_float(memory, frame_address + static_cast<std::uint32_t>((2 * kRenderFrameSamples + i) * 4), 0.25f);
    write_be_float(memory, frame_address + static_cast<std::uint32_t>((3 * kRenderFrameSamples + i) * 4), 1.0f); // LFE ignored
    write_be_float(memory, frame_address + static_cast<std::uint32_t>((4 * kRenderFrameSamples + i) * 4), 0.25f);
    write_be_float(memory, frame_address + static_cast<std::uint32_t>((5 * kRenderFrameSamples + i) * 4), 0.0f);
  }

  const auto client = audio.register_render_client(0, 0);
  assert(client);
  assert(audio.submit_render_frame(*client, frame_address));

  // Consume one Xbox render frame in two host callbacks to regress the partial
  // frame offset path. The render clock must advance through every callback.
  std::array<float, 128 * 2> first{};
  fake->pump(first);
  assert(audio.render_driver_tic() == 128);
  assert(first[0] > first[1]);
  std::array<float, 128 * 2> second{};
  fake->pump(second);
  assert(audio.render_driver_tic() == 256);
  assert(second[0] > second[1]);

  // With an active client but no queued frame, silence is consumed and still
  // advances XAudioGetRenderDriverTic while incrementing the underrun counter.
  const auto underruns = audio.underrun_count();
  std::array<float, 64 * 2> silence{};
  fake->pump(silence);
  assert(audio.render_driver_tic() == 320);
  assert(audio.underrun_count() == underruns + 1);

  // Generic voices are additive with the render-driver path rather than being
  // disabled whenever a render client exists.
  const auto voice = audio.mixer().create_voice(AudioFormat{48000, 1});
  VoiceBuffer voice_buffer{};
  voice_buffer.samples.assign(64, 0.25f);
  voice_buffer.channels = 1;
  assert(audio.mixer().submit(voice, std::move(voice_buffer)));
  assert(audio.mixer().start(voice));
  std::array<float, 64 * 2> mixed{};
  fake->pump(mixed);
  assert(mixed[0] > 0.0f && mixed[1] > 0.0f);

  assert(audio.unregister_render_client(*client));
  audio.shutdown();
  assert(memory.release(frame_address));
  std::cout << "Audio system tests passed\n";
  return 0;
}
