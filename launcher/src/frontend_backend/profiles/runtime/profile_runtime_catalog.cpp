#include "profile_runtime_catalog.hpp"

#include "../../runtime/runtime_feature.hpp"
#include "../../settings/schema/settings_catalog.hpp"
#include "../../settings/settings_feature.hpp"

#include <QList>

namespace xenon::launcher::frontend_backend {
namespace {

struct RuntimeSettingMeta {
  const char* key;
  const char* title;
  const char* description;
};

const QList<RuntimeSettingMeta>& metadata() {
  static const QList<RuntimeSettingMeta> values{
      {"runtime/graphicsBackend", "Renderer", "Override the launcher renderer for games started with this profile."},
      {"graphics/shaderCache", "Shader cache", "Enable or disable shader caching for this profile."},
      {"graphics/shaderCacheMode", "Shader cache policy", "Choose whether this profile keeps shader caches between sessions."},
      {"input/preferredDevice", "Preferred input", "Request a controller or keyboard and mouse instead of the launcher default."},
      {"input/deadzone", "Controller deadzone", "Override the default analogue-stick deadzone."},
      {"input/rumble", "Controller rumble", "Enable or disable vibration for this profile."},
      {"audio/masterVolume", "Master volume", "Set a profile-specific game audio level."},
      {"audio/muteUnfocused", "Mute when unfocused", "Mute game audio when the Xenon game window is not focused."},
      {"audio/latencyProfile", "Audio latency", "Choose a latency/stability preference for this profile."},
  };
  return values;
}

QVariantMap withMeta(QVariantMap definition, const RuntimeSettingMeta& meta) {
  definition.insert(QStringLiteral("key"), QString::fromLatin1(meta.key));
  definition.insert(QStringLiteral("title"), QString::fromLatin1(meta.title));
  definition.insert(QStringLiteral("description"), QString::fromLatin1(meta.description));
  return definition;
}

}  // namespace

QStringList ProfileRuntimeCatalog::keys() {
  QStringList result;
  result.reserve(metadata().size());
  for (const auto& meta : metadata()) result.append(QString::fromLatin1(meta.key));
  return result;
}

QVariantList ProfileRuntimeCatalog::definitions(const SettingsFeature& settings,
                                                const RuntimeFeature& runtime,
                                                bool test_mode) {
  QVariantList result;
  result.reserve(metadata().size());
  for (const auto& meta : metadata()) {
    const auto key = QString::fromLatin1(meta.key);
    auto definition = withMeta(settings.definition(key), meta);
    if (key == QStringLiteral("runtime/graphicsBackend")) {
      QVariantList filtered;
      for (const auto& backend : runtime.availableGraphicsBackends(test_mode)) {
        filtered.append(QVariantMap{{QStringLiteral("label"), backend},
                                    {QStringLiteral("value"), backend}});
      }
      definition.insert(QStringLiteral("options"), filtered);
    }
    definition.insert(QStringLiteral("effectiveDefault"), settings.value(key, definition.value(QStringLiteral("default"))));
    result.append(definition);
  }
  return result;
}

QVariantMap ProfileRuntimeCatalog::defaults(const SettingsFeature& settings) {
  QVariantMap result;
  for (const auto& key : keys()) {
    result.insert(key, settings.value(key, SettingsCatalog::defaultValue(key)));
  }
  return result;
}

QVariantMap ProfileRuntimeCatalog::resolved(const QVariantMap& profile,
                                            const SettingsFeature& settings) {
  auto result = defaults(settings);
  if (!profile.value(QStringLiteral("isolatedSettings")).toBool()) return result;
  const auto overrides = profile.value(QStringLiteral("runtimeOverrides")).toMap();
  for (const auto& key : keys()) {
    if (overrides.contains(key)) result.insert(key, overrides.value(key));
  }
  return result;
}

ServiceResult ProfileRuntimeCatalog::normalize(const QVariantMap& values,
                                               const RuntimeFeature& runtime,
                                               bool test_mode) {
  QVariantMap normalized;
  const auto allowed = keys();
  for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
    if (!allowed.contains(it.key())) {
      return ServiceResult::failure(
          QStringLiteral("Invalid profile runtime setting"),
          QStringLiteral("The profile contains an unsupported runtime override: %1").arg(it.key()));
    }
    const auto value = SettingsCatalog::normalize(it.key(), it.value());
    if (!value.ok) return value;
    if (it.key() == QStringLiteral("runtime/graphicsBackend") &&
        !runtime.availableGraphicsBackends(test_mode).contains(value.data.toString())) {
      return ServiceResult::failure(
          QStringLiteral("Renderer unavailable"),
          QStringLiteral("%1 is not available in this Xenon build on this host.").arg(value.data.toString()));
    }
    normalized.insert(it.key(), value.data);
  }
  return ServiceResult::success({}, {}, normalized);
}

}  // namespace xenon::launcher::frontend_backend
