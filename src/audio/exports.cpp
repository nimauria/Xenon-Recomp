#include "xenon/audio/exports.hpp"

#include <array>
#include <bit>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <utility>

#include "xenon/audio/system.hpp"
#include "xenon/core/export_registry.hpp"
#include "xenon/cpu/memory_port.hpp"

namespace xenon::audio {
namespace {

constexpr std::uint32_t kSuccess = 0x00000000u;
constexpr std::uint32_t kEFail = 0x80004005u;
constexpr std::uint32_t kInvalidArgument = 0x80070057u;
constexpr std::uint32_t kNoMemory = 0xC0000017u;

namespace ordinal {
constexpr std::uint32_t XAudioRenderDriverInitialize = 0x1F2;
constexpr std::uint32_t XAudioRegisterRenderDriverClient = 0x1F3;
constexpr std::uint32_t XAudioUnregisterRenderDriverClient = 0x1F4;
constexpr std::uint32_t XAudioSubmitRenderDriverFrame = 0x1F5;
constexpr std::uint32_t XAudioGetVoiceCategoryVolumeChangeMask = 0x1F7;
constexpr std::uint32_t XAudioGetVoiceCategoryVolume = 0x1F8;
constexpr std::uint32_t XAudioSetVoiceCategoryVolume = 0x1F9;
constexpr std::uint32_t XAudioGetSpeakerConfig = 0x1FF;
constexpr std::uint32_t XAudioSetSpeakerConfig = 0x200;
constexpr std::uint32_t XMACreateContext = 0x224;
constexpr std::uint32_t XMAInitializeContext = 0x225;
constexpr std::uint32_t XMAReleaseContext = 0x226;
constexpr std::uint32_t XMAEnableContext = 0x227;
constexpr std::uint32_t XMADisableContext = 0x228;
constexpr std::uint32_t XMAGetOutputBufferWriteOffset = 0x229;
constexpr std::uint32_t XMASetOutputBufferReadOffset = 0x22A;
constexpr std::uint32_t XMAGetOutputBufferReadOffset = 0x22B;
constexpr std::uint32_t XMASetOutputBufferValid = 0x22C;
constexpr std::uint32_t XMAIsOutputBufferValid = 0x22D;
constexpr std::uint32_t XMASetInputBuffer0Valid = 0x22E;
constexpr std::uint32_t XMAIsInputBuffer0Valid = 0x22F;
constexpr std::uint32_t XMASetInputBuffer1Valid = 0x230;
constexpr std::uint32_t XMAIsInputBuffer1Valid = 0x231;
constexpr std::uint32_t XMASetInputBuffer0 = 0x232;
constexpr std::uint32_t XMASetInputBuffer1 = 0x233;
constexpr std::uint32_t XMAGetPacketMetadata = 0x234;
constexpr std::uint32_t XMABlockWhileInUse = 0x235;
constexpr std::uint32_t XMASetLoopData = 0x236;
constexpr std::uint32_t XMASetInputBufferReadOffset = 0x237;
constexpr std::uint32_t XMAGetInputBufferReadOffset = 0x238;
constexpr std::uint32_t XAudioSuspendRenderDriverClients = 0x2D4;
constexpr std::uint32_t XAudioGetRenderDriverTic = 0x34C;
constexpr std::uint32_t XAudioEnableDucker = 0x34D;
constexpr std::uint32_t XAudioSetDuckerLevel = 0x34E;
constexpr std::uint32_t XAudioIsDuckerEnabled = 0x34F;
constexpr std::uint32_t XAudioGetDuckerLevel = 0x350;
constexpr std::uint32_t XAudioGetDuckerThreshold = 0x351;
constexpr std::uint32_t XAudioSetDuckerThreshold = 0x352;
constexpr std::uint32_t XAudioGetDuckerAttackTime = 0x353;
constexpr std::uint32_t XAudioSetDuckerAttackTime = 0x354;
constexpr std::uint32_t XAudioGetDuckerReleaseTime = 0x355;
constexpr std::uint32_t XAudioSetDuckerReleaseTime = 0x356;
constexpr std::uint32_t XAudioGetDuckerHoldTime = 0x357;
constexpr std::uint32_t XAudioSetDuckerHoldTime = 0x358;
constexpr std::uint32_t XAudioGetUnderrunCount = 0x35A;
}  // namespace ordinal

void set_result(core::ExportCallContext& ctx, std::uint32_t value) noexcept {
  ctx.cpu.gpr[3] = value;
}

[[nodiscard]] float argument_float(const core::ExportCallContext& ctx) noexcept {
  return static_cast<float>(ctx.cpu.fpr(1));
}

void set_float_result(core::ExportCallContext& ctx, float value) noexcept {
  ctx.cpu.set_fpr(1, static_cast<double>(value));
}

void write_float_be(cpu::MemoryPort& memory, cpu::GuestAddress address, float value) {
  memory.write32_be(address, std::bit_cast<std::uint32_t>(value));
}

[[nodiscard]] bool load_xma_init(cpu::MemoryPort& memory, cpu::GuestAddress address,
                                 XmaContextInit& out) {
  if (!address) return false;
  out.input_buffer_0 = memory.read32_be(address + 0);
  out.input_buffer_0_packet_count = memory.read32_be(address + 4);
  out.input_buffer_1 = memory.read32_be(address + 8);
  out.input_buffer_1_packet_count = memory.read32_be(address + 12);
  out.input_buffer_read_offset = memory.read32_be(address + 16);
  out.output_buffer = memory.read32_be(address + 20);
  out.output_buffer_block_count = memory.read32_be(address + 24);
  out.work_buffer = memory.read32_be(address + 28);
  out.subframe_decode_count = memory.read32_be(address + 32);
  out.channel_count = memory.read32_be(address + 36);
  out.sample_rate = memory.read32_be(address + 40);
  out.loop_start = memory.read32_be(address + 44);
  out.loop_end = memory.read32_be(address + 48);
  out.loop_count = memory.read8(address + 52);
  out.loop_subframe_end = memory.read8(address + 53);
  out.loop_subframe_skip = memory.read8(address + 54);
  return true;
}

[[nodiscard]] bool mutate_context(XmaDecoder& xma, cpu::GuestAddress address,
                                  const std::function<void(XmaContextData&)>& fn) {
  XmaContextData data{};
  if (!xma.read_context(address, data)) return false;
  fn(data);
  // Guest API setters are CPU-originated writes. They still go through
  // MemoryPort, while asynchronous decoder progress uses write_physical() and
  // therefore participates in Memory V2 reservation/coherency publication.
  return xma.write_context(address, data, false);
}

bool add_export(core::ExportRegistry& registry, std::string name,
                std::uint32_t export_ordinal, core::ExportHandler handler) {
  core::ExportDescriptor descriptor{};
  descriptor.library = "xboxkrnl";
  descriptor.name = std::move(name);
  descriptor.ordinal = export_ordinal;
  descriptor.requirement = core::ExportRequirement::Required;
  descriptor.handler = std::move(handler);
  return registry.register_export(std::move(descriptor));
}

}  // namespace

bool register_xbox_audio_exports(core::ExportRegistry& registry,
                                 AudioSystem& audio) {
  bool ok = true;
  auto add = [&](std::string name, std::uint32_t value,
                 core::ExportHandler handler) {
    ok = add_export(registry, std::move(name), value, std::move(handler)) && ok;
  };

  add("XAudioRenderDriverInitialize", ordinal::XAudioRenderDriverInitialize,
      [&audio](core::ExportCallContext& ctx) {
        set_result(ctx, audio.initialized() ? kSuccess : kEFail);
        return true;
      });

  add("XAudioRegisterRenderDriverClient", ordinal::XAudioRegisterRenderDriverClient,
      [&audio](core::ExportCallContext& ctx) {
        const auto callback_pair = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
        const auto driver_out = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
        if (!callback_pair || !driver_out) {
          set_result(ctx, kInvalidArgument);
          return true;
        }
        const auto callback = ctx.memory.read32_be(callback_pair);
        const auto callback_arg = ctx.memory.read32_be(callback_pair + 4u);
        if (!callback) {
          set_result(ctx, kInvalidArgument);
          return true;
        }
        const auto handle = audio.register_render_client(callback, callback_arg);
        if (!handle) {
          set_result(ctx, kNoMemory);
          return true;
        }
        ctx.memory.write32_be(driver_out, *handle);
        set_result(ctx, kSuccess);
        return true;
      });

  add("XAudioUnregisterRenderDriverClient", ordinal::XAudioUnregisterRenderDriverClient,
      [&audio](core::ExportCallContext& ctx) {
        const auto handle = static_cast<RenderClientHandle>(ctx.cpu.gpr[3]);
        set_result(ctx, audio.unregister_render_client(handle) ? kSuccess : kInvalidArgument);
        return true;
      });

  add("XAudioSubmitRenderDriverFrame", ordinal::XAudioSubmitRenderDriverFrame,
      [&audio](core::ExportCallContext& ctx) {
        const auto handle = static_cast<RenderClientHandle>(ctx.cpu.gpr[3]);
        const auto samples = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
        set_result(ctx, audio.submit_render_frame(handle, samples) ? kSuccess : kEFail);
        return true;
      });

  add("XAudioGetVoiceCategoryVolumeChangeMask",
      ordinal::XAudioGetVoiceCategoryVolumeChangeMask,
      [&audio](core::ExportCallContext& ctx) {
        const auto driver = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
        const auto out = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
        if ((driver & 0xFFFF0000u) != 0x41550000u || !out) {
          set_result(ctx, kInvalidArgument);
          return true;
        }
        ctx.memory.write32_be(out, audio.voice_category_change_mask());
        set_result(ctx, kSuccess);
        return true;
      });

  add("XAudioGetVoiceCategoryVolume", ordinal::XAudioGetVoiceCategoryVolume,
      [&audio](core::ExportCallContext& ctx) {
        const auto category = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
        const auto out = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
        if (!out) {
          set_result(ctx, kInvalidArgument);
          return true;
        }
        write_float_be(ctx.memory, out, audio.voice_category_volume(category));
        set_result(ctx, kSuccess);
        return true;
      });

  add("XAudioSetVoiceCategoryVolume", ordinal::XAudioSetVoiceCategoryVolume,
      [&audio](core::ExportCallContext& ctx) {
        const auto category = static_cast<std::uint32_t>(ctx.cpu.gpr[3]);
        const float value = argument_float(ctx);
        set_result(ctx, audio.set_voice_category_volume(category, value) ? kSuccess
                                                                         : kInvalidArgument);
        return true;
      });

  add("XAudioGetSpeakerConfig", ordinal::XAudioGetSpeakerConfig,
      [&audio](core::ExportCallContext& ctx) {
        const auto out = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
        if (!out) {
          set_result(ctx, kInvalidArgument);
          return true;
        }
        ctx.memory.write32_be(out, audio.speaker_config());
        set_result(ctx, kSuccess);
        return true;
      });

  add("XAudioSetSpeakerConfig", ordinal::XAudioSetSpeakerConfig,
      [&audio](core::ExportCallContext& ctx) {
        audio.set_speaker_config(static_cast<std::uint32_t>(ctx.cpu.gpr[3]));
        set_result(ctx, kSuccess);
        return true;
      });

  add("XMACreateContext", ordinal::XMACreateContext,
      [&audio](core::ExportCallContext& ctx) {
        const auto out = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
        if (!out) {
          set_result(ctx, kInvalidArgument);
          return true;
        }
        const auto context = audio.xma().allocate_context();
        if (!context) {
          set_result(ctx, kNoMemory);
          return true;
        }
        ctx.memory.write32_be(out, context);
        set_result(ctx, kSuccess);
        return true;
      });

  add("XMAInitializeContext", ordinal::XMAInitializeContext,
      [&audio](core::ExportCallContext& ctx) {
        const auto context = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
        const auto init_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
        XmaContextInit init{};
        if (!load_xma_init(ctx.memory, init_ptr, init) ||
            !audio.xma().initialize_context(context, init)) {
          set_result(ctx, kEFail);
          return true;
        }
        set_result(ctx, kSuccess);
        return true;
      });

  add("XMAReleaseContext", ordinal::XMAReleaseContext,
      [&audio](core::ExportCallContext& ctx) {
        const auto context = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
        set_result(ctx, audio.xma().release_context(context) ? kSuccess : kInvalidArgument);
        return true;
      });

  add("XMAEnableContext", ordinal::XMAEnableContext,
      [&audio](core::ExportCallContext& ctx) {
        const auto context = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
        set_result(ctx, audio.xma().enable_context(context) ? kSuccess : kInvalidArgument);
        return true;
      });

  add("XMADisableContext", ordinal::XMADisableContext,
      [&audio](core::ExportCallContext& ctx) {
        const auto context = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
        const bool wait = ctx.cpu.gpr[4] != 0;
        set_result(ctx, audio.xma().disable_context(context, wait) ? kSuccess : kEFail);
        return true;
      });

  auto get_context_field = [&](std::string name, std::uint32_t value,
                               auto field) {
    add(std::move(name), value,
        [&audio, field](core::ExportCallContext& ctx) {
          const auto context = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
          XmaContextData data{};
          if (!audio.xma().read_context(context, data)) {
            set_result(ctx, 0);
            return true;
          }
          set_result(ctx, static_cast<std::uint32_t>(data.*field));
          return true;
        });
  };

  get_context_field("XMAGetOutputBufferWriteOffset",
                    ordinal::XMAGetOutputBufferWriteOffset,
                    &XmaContextData::output_buffer_write_offset);
  get_context_field("XMAGetOutputBufferReadOffset",
                    ordinal::XMAGetOutputBufferReadOffset,
                    &XmaContextData::output_buffer_read_offset);
  get_context_field("XMAIsOutputBufferValid", ordinal::XMAIsOutputBufferValid,
                    &XmaContextData::output_buffer_valid);
  get_context_field("XMAIsInputBuffer0Valid", ordinal::XMAIsInputBuffer0Valid,
                    &XmaContextData::input_buffer_0_valid);
  get_context_field("XMAIsInputBuffer1Valid", ordinal::XMAIsInputBuffer1Valid,
                    &XmaContextData::input_buffer_1_valid);
  get_context_field("XMAGetPacketMetadata", ordinal::XMAGetPacketMetadata,
                    &XmaContextData::packet_metadata);
  get_context_field("XMAGetInputBufferReadOffset",
                    ordinal::XMAGetInputBufferReadOffset,
                    &XmaContextData::input_buffer_read_offset);

  add("XMASetOutputBufferReadOffset", ordinal::XMASetOutputBufferReadOffset,
      [&audio](core::ExportCallContext& ctx) {
        const auto context = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
        const auto value = static_cast<std::uint32_t>(ctx.cpu.gpr[4]);
        set_result(ctx, mutate_context(audio.xma(), context, [value](XmaContextData& data) {
                     data.output_buffer_read_offset = static_cast<std::uint8_t>(value & 0x1Fu);
                   }) ? kSuccess : kInvalidArgument);
        return true;
      });

  add("XMASetOutputBufferValid", ordinal::XMASetOutputBufferValid,
      [&audio](core::ExportCallContext& ctx) {
        const auto context = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
        set_result(ctx, mutate_context(audio.xma(), context, [](XmaContextData& data) {
                     data.output_buffer_valid = true;
                   }) ? kSuccess : kInvalidArgument);
        return true;
      });

  add("XMASetInputBuffer0Valid", ordinal::XMASetInputBuffer0Valid,
      [&audio](core::ExportCallContext& ctx) {
        const auto context = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
        set_result(ctx, mutate_context(audio.xma(), context, [](XmaContextData& data) {
                     data.input_buffer_0_valid = true;
                   }) ? kSuccess : kInvalidArgument);
        return true;
      });

  add("XMASetInputBuffer1Valid", ordinal::XMASetInputBuffer1Valid,
      [&audio](core::ExportCallContext& ctx) {
        const auto context = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
        set_result(ctx, mutate_context(audio.xma(), context, [](XmaContextData& data) {
                     data.input_buffer_1_valid = true;
                   }) ? kSuccess : kInvalidArgument);
        return true;
      });

  const auto set_input_buffer = [&audio](core::ExportCallContext& ctx, bool second) {
    const auto context = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
    const auto buffer = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
    const auto packet_count = static_cast<std::uint32_t>(ctx.cpu.gpr[5]);
    if (packet_count > 0xFFFu) {
      set_result(ctx, kInvalidArgument);
      return true;
    }
    // Translate through the guest MemoryPort's owning AddressSpace indirectly
    // by updating an XmaContextInit-equivalent field through XmaDecoder. The
    // decoder owns the physical context representation, so use the public
    // AddressSpace-backed helper by translating the pointer with Memory V2.
    auto* address_space = dynamic_cast<memory::AddressSpace*>(&ctx.memory);
    if (!address_space) {
      set_result(ctx, kEFail);
      return true;
    }
    const std::uint32_t translated = buffer ? address_space->get_physical_address(buffer) : 0u;
    if (buffer && translated == 0xFFFFFFFFu) {
      set_result(ctx, kEFail);
      return true;
    }
    const bool changed = mutate_context(audio.xma(), context,
        [second, translated, packet_count](XmaContextData& data) {
          if (second) {
            data.input_buffer_1_ptr = translated;
            data.input_buffer_1_packet_count = static_cast<std::uint16_t>(packet_count);
          } else {
            data.input_buffer_0_ptr = translated;
            data.input_buffer_0_packet_count = static_cast<std::uint16_t>(packet_count);
          }
        });
    set_result(ctx, changed ? kSuccess : kInvalidArgument);
    return true;
  };

  add("XMASetInputBuffer0", ordinal::XMASetInputBuffer0,
      [set_input_buffer](core::ExportCallContext& ctx) mutable {
        return set_input_buffer(ctx, false);
      });
  add("XMASetInputBuffer1", ordinal::XMASetInputBuffer1,
      [set_input_buffer](core::ExportCallContext& ctx) mutable {
        return set_input_buffer(ctx, true);
      });

  add("XMABlockWhileInUse", ordinal::XMABlockWhileInUse,
      [&audio](core::ExportCallContext& ctx) {
        const auto context = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
        set_result(ctx, audio.xma().block_while_in_use(context) ? kSuccess : kInvalidArgument);
        return true;
      });

  add("XMASetLoopData", ordinal::XMASetLoopData,
      [&audio](core::ExportCallContext& ctx) {
        const auto context = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
        const auto loop = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[4]);
        if (!loop) {
          set_result(ctx, kInvalidArgument);
          return true;
        }
        // XMASetLoopData receives a pointer to XMA_CONTEXT_DATA, not the
        // compact 12-byte loop substructure embedded in XMA_CONTEXT_INIT.
        // Decode the canonical 64-byte hardware layout so the packed loop
        // fields are read from the same bit positions the guest/XMA unit uses.
        std::array<std::byte, kXmaContextBytes> loop_bytes{};
        ctx.memory.read_bytes(loop, loop_bytes);
        const auto loop_data = XmaContextData::decode(loop_bytes);
        const bool changed = mutate_context(audio.xma(), context,
            [&loop_data](XmaContextData& data) {
              data.loop_start = loop_data.loop_start;
              data.loop_end = loop_data.loop_end;
              data.loop_count = loop_data.loop_count;
              data.loop_subframe_end = loop_data.loop_subframe_end;
              data.loop_subframe_skip = loop_data.loop_subframe_skip;
            });
        set_result(ctx, changed ? kSuccess : kInvalidArgument);
        return true;
      });

  add("XMASetInputBufferReadOffset", ordinal::XMASetInputBufferReadOffset,
      [&audio](core::ExportCallContext& ctx) {
        const auto context = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[3]);
        const auto value = static_cast<std::uint32_t>(ctx.cpu.gpr[4]);
        set_result(ctx, mutate_context(audio.xma(), context, [value](XmaContextData& data) {
                     data.input_buffer_read_offset = value & 0x03FFFFFFu;
                   }) ? kSuccess : kInvalidArgument);
        return true;
      });

  add("XAudioSuspendRenderDriverClients", ordinal::XAudioSuspendRenderDriverClients,
      [&audio](core::ExportCallContext& ctx) {
        // Suspending with no registered clients is still a valid global state
        // transition, so this export succeeds independently of client count.
        (void)audio.suspend_render_clients(ctx.cpu.gpr[3] != 0);
        set_result(ctx, kSuccess);
        return true;
      });

  add("XAudioGetRenderDriverTic", ordinal::XAudioGetRenderDriverTic,
      [&audio](core::ExportCallContext& ctx) {
        set_result(ctx, audio.render_driver_tic());
        return true;
      });

  add("XAudioEnableDucker", ordinal::XAudioEnableDucker,
      [&audio](core::ExportCallContext& ctx) {
        audio.enable_ducker(ctx.cpu.gpr[3] != 0);
        set_result(ctx, kSuccess);
        return true;
      });
  add("XAudioSetDuckerLevel", ordinal::XAudioSetDuckerLevel,
      [&audio](core::ExportCallContext& ctx) {
        audio.set_ducker_level(argument_float(ctx));
        set_result(ctx, kSuccess);
        return true;
      });
  add("XAudioIsDuckerEnabled", ordinal::XAudioIsDuckerEnabled,
      [&audio](core::ExportCallContext& ctx) {
        set_result(ctx, audio.ducker_enabled() ? 1u : 0u);
        return true;
      });
  add("XAudioGetDuckerLevel", ordinal::XAudioGetDuckerLevel,
      [&audio](core::ExportCallContext& ctx) {
        set_float_result(ctx, audio.ducker_level());
        return true;
      });
  add("XAudioGetDuckerThreshold", ordinal::XAudioGetDuckerThreshold,
      [&audio](core::ExportCallContext& ctx) {
        set_float_result(ctx, audio.ducker_threshold());
        return true;
      });
  add("XAudioSetDuckerThreshold", ordinal::XAudioSetDuckerThreshold,
      [&audio](core::ExportCallContext& ctx) {
        audio.set_ducker_threshold(argument_float(ctx));
        set_result(ctx, kSuccess);
        return true;
      });
  add("XAudioGetDuckerAttackTime", ordinal::XAudioGetDuckerAttackTime,
      [&audio](core::ExportCallContext& ctx) {
        set_float_result(ctx, audio.ducker_attack());
        return true;
      });
  add("XAudioSetDuckerAttackTime", ordinal::XAudioSetDuckerAttackTime,
      [&audio](core::ExportCallContext& ctx) {
        audio.set_ducker_attack(argument_float(ctx));
        set_result(ctx, kSuccess);
        return true;
      });
  add("XAudioGetDuckerReleaseTime", ordinal::XAudioGetDuckerReleaseTime,
      [&audio](core::ExportCallContext& ctx) {
        set_float_result(ctx, audio.ducker_release());
        return true;
      });
  add("XAudioSetDuckerReleaseTime", ordinal::XAudioSetDuckerReleaseTime,
      [&audio](core::ExportCallContext& ctx) {
        audio.set_ducker_release(argument_float(ctx));
        set_result(ctx, kSuccess);
        return true;
      });
  add("XAudioGetDuckerHoldTime", ordinal::XAudioGetDuckerHoldTime,
      [&audio](core::ExportCallContext& ctx) {
        set_float_result(ctx, audio.ducker_hold());
        return true;
      });
  add("XAudioSetDuckerHoldTime", ordinal::XAudioSetDuckerHoldTime,
      [&audio](core::ExportCallContext& ctx) {
        audio.set_ducker_hold(argument_float(ctx));
        set_result(ctx, kSuccess);
        return true;
      });
  add("XAudioGetUnderrunCount", ordinal::XAudioGetUnderrunCount,
      [&audio](core::ExportCallContext& ctx) {
        set_result(ctx, audio.underrun_count());
        return true;
      });

  return ok;
}

}  // namespace xenon::audio
