#pragma once

namespace xenon::core { class ExportRegistry; }
namespace xenon::audio { class AudioSystem; }

namespace xenon::audio {

[[nodiscard]] bool register_xbox_audio_exports(core::ExportRegistry& registry,
                                               AudioSystem& audio);

}  // namespace xenon::audio
