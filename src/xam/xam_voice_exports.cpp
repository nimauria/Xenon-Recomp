#include "xenon/core/export_registry.hpp"
#include "xenon/xam/xam_exports.hpp"
#include "xenon/xam/types.hpp"

namespace xenon::xam {

// XamVoice* (0x030C-0x030F) - Xbox 360 USB/wireless headset voice chat.
// Xenon models no voice-capture hardware, so these report the real "no
// headset device" outcome consistently rather than fabricating a working
// voice channel. Verified against rexglue-sdk's xam_voice.cpp (itself
// crediting xenia): XamVoiceCreate zeroes its output handle and returns
// X_ERROR_ACCESS_DENIED (the real result when no voice device exists to
// grant access to); XamVoiceClose/XamVoiceHeadsetPresent both return 0
// (close trivially succeeds even for a handle that was never really created;
// "is a headset present" is honestly false). XamVoiceSubmitPacket has no
// real reference implementation anywhere available (even rexglue-sdk leaves
// it an unimplemented stub) - since XamVoiceCreate above never hands out a
// real handle, any handle a title passes here is necessarily invalid, so
// this reports that directly instead of silently accepting bad input.
bool register_voice_exports(core::ExportRegistry& registry) {
  bool ok = true;

  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamVoiceCreate";
    desc.ordinal = ordinal::XamVoiceCreate;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      const auto out_voice_ptr = static_cast<cpu::GuestAddress>(ctx.cpu.gpr[5]);
      if (out_voice_ptr != 0u) ctx.memory.write32_be(out_voice_ptr, 0u);
      ctx.cpu.gpr[3] = result::AccessDenied;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamVoiceClose";
    desc.ordinal = ordinal::XamVoiceClose;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = result::Success;
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamVoiceHeadsetPresent";
    desc.ordinal = ordinal::XamVoiceHeadsetPresent;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = 0u;  // false: no headset present
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }
  {
    core::ExportDescriptor desc{};
    desc.library = "xam";
    desc.name = "XamVoiceSubmitPacket";
    desc.ordinal = ordinal::XamVoiceSubmitPacket;
    desc.requirement = core::ExportRequirement::Required;
    desc.handler = [](core::ExportCallContext& ctx) -> bool {
      ctx.cpu.gpr[3] = result::InvalidParameter;  // no real handle could ever be valid
      return true;
    };
    ok = registry.register_export(std::move(desc)) && ok;
  }

  return ok;
}

}  // namespace xenon::xam
