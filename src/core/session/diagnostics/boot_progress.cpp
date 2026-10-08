#include <algorithm>
#include <cctype>
#include <string>

#include "xenon/core/session.hpp"

namespace xenon::core {

void XenonSession::reach_boot_checkpoint(BootCheckpoint checkpoint) {
  if (!boot_checkpoints_.reach(checkpoint)) return;
  logging::Logger::instance().log_if_enabled(
      logging::Level::Info, "boot", [&] {
        return std::string("checkpoint: ") + std::string(to_string(checkpoint));
      });
}

// Part 15 of the AC6 Runtime Readiness pass ("boot phase checkpoints"):
// closes the gap between the 3 checkpoints wired at their own direct call
// sites (XexLoaded, EntryStarted, FirstGuestThread - none of which are
// export calls) and the remaining ones, which are all first-observed
// through a specific real guest export call. Ordinals are the same real,
// already-verified ones their own export registration files use (see the
// comment at each case) - never guessed.
void XenonSession::observe_boot_checkpoint_from_export_call(
    std::string_view module, std::uint32_t ordinal, const cpu::CpuState& state) {
  // Real XEX import tables spell library names "xboxkrnl.exe"/"xam.xex" -
  // ExportRegistry::normalize_library() does the same lowercase+strip
  // internally but is private, so this matches its exact behavior locally
  // rather than comparing against the wrong (suffixed/cased) string.
  std::string normalized_module(module);
  std::transform(normalized_module.begin(), normalized_module.end(),
                 normalized_module.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (normalized_module.ends_with(".exe") || normalized_module.ends_with(".xex")) {
    normalized_module.resize(normalized_module.size() - 4);
  }
  if (normalized_module == "xboxkrnl") {
    switch (ordinal) {
      case 0x00D2u:  // NtCreateFile - src/xbox/exports/xboxkrnl_io_exports.cpp
      case 0x00DFu:  // NtOpenFile - src/xbox/exports/xboxkrnl_io_exports.cpp
        reach_boot_checkpoint(BootCheckpoint::FirstFileOpen);
        return;
      case 0x1F3u:  // XAudioRegisterRenderDriverClient - src/audio/exports.cpp
        reach_boot_checkpoint(BootCheckpoint::FirstAudioClient);
        return;
      default:
        return;
    }
  }
  if (normalized_module == "xam") {
    switch (ordinal) {
      case 0x0191u:  // XamInputGetState - include/xenon/xam/xam_exports.hpp
        reach_boot_checkpoint(BootCheckpoint::FirstInputPoll);
        return;
      case 0x0210u:  // XamUserGetSigninState - include/xenon/xam/xam_exports.hpp
        // gpr[3] carries the real xam::SigninState the handler wrote
        // (NotSignedIn=0) - "ready" means an actual signed-in profile, not
        // merely that the guest asked whether one exists.
        if (state.gpr[3] != 0u) {
          reach_boot_checkpoint(BootCheckpoint::ProfileReady);
        }
        return;
      case 0x025Cu:  // XamContentCreateEnumerator - include/xenon/xam/xam_exports.hpp
        reach_boot_checkpoint(BootCheckpoint::SaveEnumeration);
        return;
      default:
        return;
    }
  }
}

}  // namespace xenon::core
