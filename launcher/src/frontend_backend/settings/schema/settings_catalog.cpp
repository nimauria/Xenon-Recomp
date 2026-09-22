#include "settings_catalog.hpp"

#include <QList>
#include <QMetaType>
#include <QtGlobal>

#include <algorithm>
#include <initializer_list>

namespace xenon::launcher::frontend_backend {
namespace {

struct Option {
  const char* label;
  QVariant value;
};

struct Definition {
  const char* key;
  const char* category;
  const char* type;
  QVariant default_value;
  QVariantList options;
  double minimum = 0.0;
  double maximum = 0.0;
  bool has_range = false;
  bool requires_restart = false;
};

QVariantMap option(const QString& label, const QVariant& value) {
  return {{QStringLiteral("label"), label}, {QStringLiteral("value"), value}};
}

QVariantList stringOptions(std::initializer_list<const char*> values) {
  QVariantList result;
  result.reserve(static_cast<qsizetype>(values.size()));
  for (const auto* value : values) {
    const auto text = QString::fromLatin1(value);
    result.push_back(option(text, text));
  }
  return result;
}

const QList<Definition>& definitions() {
  static const QList<Definition> values{
      {"general/startupPage", "general", "enum", QStringLiteral("Library"),
       stringOptions({"Home", "Library", "Downloads", "Modules", "Profiles", "Captures", "Network", "Support", "Settings"})},
      {"general/sidebarMode", "general", "enum", QStringLiteral("Auto"),
       stringOptions({"Auto", "Expanded", "Compact"})},
      {"general/restoreLastPage", "general", "bool", false, {}},
      {"general/animations", "general", "bool", true, {}},
      {"general/compact", "appearance", "bool", false, {}},

      {"system/rememberWindowGeometry", "system", "bool", true, {}},
      {"system/restoreMaximized", "system", "bool", true, {}},
      {"system/startMinimized", "system", "bool", false, {}},

      {"appearance/themeBackdrop", "appearance", "bool", true, {}},
      {"appearance/backdropIntensity", "appearance", "number", 0.72,
       {option(QStringLiteral("Subtle"), 0.42), option(QStringLiteral("Standard"), 0.72),
        option(QStringLiteral("Strong"), 1.0)}, 0.20, 1.0, true},
      {"appearance/artworkBackgrounds", "appearance", "bool", true, {}},
      {"appearance/decorLevel", "appearance", "enum", QStringLiteral("Balanced"),
       stringOptions({"Minimal", "Balanced", "Full"})},
      {"appearance/panelOpacity", "appearance", "number", 0.94,
       {option(QStringLiteral("0%"), 0.0), option(QStringLiteral("25%"), 0.25),
        option(QStringLiteral("50%"), 0.50), option(QStringLiteral("75%"), 0.75),
        option(QStringLiteral("100%"), 1.0)}, 0.0, 1.0, true},

      {"library/showCompatibility", "library", "bool", true, {}},
      {"library/missingContent", "library", "enum", QStringLiteral("Show in catalogue"),
       stringOptions({"Show in catalogue", "Hide missing content"})},
      {"library/filter", "library", "enum", QStringLiteral("All Games"),
       stringOptions({"All Games", "Installed", "Ready to Play", "Needs Attention", "Module Disabled", "Update Available", "Favorites"})},
      {"library/sort", "library", "enum", QStringLiteral("Recently Played"),
       stringOptions({"Recently Played", "Recently Added", "A-Z", "Z-A", "Playtime: High to Low", "Playtime: Low to High", "Favorites First", "Updates First"})},
      {"library/viewMode", "library", "enum", QStringLiteral("Focused"),
       stringOptions({"Focused", "Carousel", "Grid"})},
      {"library/gridDensity", "library", "enum", QStringLiteral("Auto"),
       stringOptions({"Auto", "3", "6"})},
      {"library/wrapNavigation", "library", "bool", true, {}},
      {"library/rememberSelection", "library", "bool", true, {}},
      // UI state keys are registered so they still pass through the settings
      // validation layer, but use an internal category so Settings does not
      // expose implementation-detail controls to users.
      {"library/lastSelectedGameId", "internal", "string", QString{}, {}},
      {"downloads/viewMode", "internal", "enum", QStringLiteral("Overview"),
       stringOptions({"Overview", "Active", "Ready", "Issues", "History"})},

      {"runtime/graphicsBackend", "runtime", "enum", QStringLiteral("Automatic"),
       stringOptions({"Automatic", "Vulkan", "Direct3D 12"})},
      {"runtime/offline", "runtime", "bool", true, {}},
      {"runtime/afterLaunch", "runtime", "enum", QStringLiteral("Keep launcher open"),
       stringOptions({"Keep launcher open", "Minimize launcher", "Close launcher"})},

      {"graphics/shaderCache", "graphics", "bool", true, {}},
      {"graphics/shaderCacheMode", "graphics", "enum", QStringLiteral("Persistent"),
       stringOptions({"Persistent", "Session only"})},

      {"input/backend", "input", "enum", QStringLiteral("Automatic"),
       stringOptions({"Automatic", "Native XInput", "SDL"})},
      {"input/preferredDevice", "input", "enum", QStringLiteral("Automatic"),
       stringOptions({"Automatic", "Controller", "Keyboard & Mouse", "Flight Stick / HOTAS", "Multiple Sources"})},
      {"input/backgroundInput", "input", "bool", false, {}},
      {"input/rumble", "input", "bool", true, {}},
      {"input/deadzone", "input", "number", 0.10,
       {option(QStringLiteral("5%"), 0.05), option(QStringLiteral("10%"), 0.10),
        option(QStringLiteral("15%"), 0.15), option(QStringLiteral("20%"), 0.20),
        option(QStringLiteral("25%"), 0.25), option(QStringLiteral("30%"), 0.30)},
       0.0, 0.50, true},

      {"audio/masterVolume", "audio", "number", 1.0,
       {option(QStringLiteral("25%"), 0.25), option(QStringLiteral("50%"), 0.50),
        option(QStringLiteral("75%"), 0.75), option(QStringLiteral("100%"), 1.0)},
       0.0, 1.0, true},
      {"audio/muteUnfocused", "audio", "bool", false, {}},
      {"audio/latencyProfile", "audio", "enum", QStringLiteral("Automatic"),
       stringOptions({"Automatic", "Low latency", "Balanced", "Stable"})},

      {"updates/automaticChecks", "updates", "bool", true, {}},
      {"updates/checkInterval", "updates", "enum", QStringLiteral("Daily"),
       stringOptions({"At startup", "Daily", "Weekly", "Manual"})},
      {"updates/prerelease", "updates", "bool", false, {}},
      {"updates/modules", "updates", "bool", true, {}},
      {"updates/modulePrerelease", "updates", "bool", false, {}},

      {"community/discordRichPresence", "community", "bool", false, {}},
      {"community/discordShowGameTitle", "community", "bool", true, {}},

      {"accessibility/textScale", "accessibility", "number", 1.0,
       {option(QStringLiteral("100%"), 1.0), option(QStringLiteral("110%"), 1.1),
        option(QStringLiteral("125%"), 1.25), option(QStringLiteral("150%"), 1.5),
        option(QStringLiteral("175%"), 1.75), option(QStringLiteral("200%"), 2.0)},
       1.0, 2.0, true},
      {"accessibility/highContrast", "accessibility", "bool", false, {}},
      {"accessibility/reduceMotion", "accessibility", "bool", false, {}},
      {"accessibility/enhancedFocus", "accessibility", "bool", false, {}},

      {"developer/fixtureMode", "developer", "enum", QStringLiteral("none"),
       {option(QStringLiteral("None"), QStringLiteral("none")),
        option(QStringLiteral("Generic Xenon UI"), QStringLiteral("generic")),
        option(QStringLiteral("Project Gracemeria UI Preview"), QStringLiteral("gracemeria"))}},
      {"developer/verboseLogging", "developer", "bool", false, {}},

      // Registered under "about" (not "developer") because the toggle for
      // it lives in Settings > About, not the hidden/kTestMode-only
      // Developer category - this is a normal user-facing feature, not an
      // internal QA tool. It gates the Developer sidebar page/entry itself.
      {"developer/modeEnabled", "about", "bool", false, {}},
  };
  return values;
}

const Definition* findDefinition(const QString& key) {
  const auto normalized = key.trimmed();
  const auto& values = definitions();
  const auto it = std::find_if(values.cbegin(), values.cend(), [&](const Definition& definition) {
    return normalized == QString::fromLatin1(definition.key);
  });
  return it == values.cend() ? nullptr : &*it;
}

bool variantEquivalent(const QVariant& lhs, const QVariant& rhs) {
  if (lhs.metaType().id() == QMetaType::Double || rhs.metaType().id() == QMetaType::Double ||
      lhs.metaType().id() == QMetaType::Float || rhs.metaType().id() == QMetaType::Float) {
    bool left_ok = false;
    bool right_ok = false;
    const auto left = lhs.toDouble(&left_ok);
    const auto right = rhs.toDouble(&right_ok);
    return left_ok && right_ok && qAbs(left - right) < 0.000001;
  }
  return lhs.toString() == rhs.toString();
}

QVariant normalizeBool(const QVariant& value, bool* ok) {
  if (value.metaType().id() == QMetaType::Bool) {
    *ok = true;
    return value.toBool();
  }
  const auto text = value.toString().trimmed().toLower();
  if (text == QStringLiteral("true") || text == QStringLiteral("1") || text == QStringLiteral("yes") ||
      text == QStringLiteral("on")) {
    *ok = true;
    return true;
  }
  if (text == QStringLiteral("false") || text == QStringLiteral("0") || text == QStringLiteral("no") ||
      text == QStringLiteral("off")) {
    *ok = true;
    return false;
  }
  *ok = false;
  return {};
}

}  // namespace

QVariantList SettingsCatalog::categories() {
  return {
      QVariantMap{{"id", "general"}, {"name", "General"}, {"feature", "settings.general"}, {"page", 0},
                  {"keywords", "startup language sidebar navigation animations"}},
      QVariantMap{{"id", "appearance"}, {"name", "Appearance"}, {"feature", "settings.appearance"}, {"page", 1},
                  {"keywords", "theme accent custom colour color background density corners transparency decor"}},
      QVariantMap{{"id", "library"}, {"name", "Library"}, {"feature", "settings.library"}, {"page", 2},
                  {"keywords", "games dlc compatibility missing content"}},
      QVariantMap{{"id", "paths"}, {"name", "Paths"}, {"feature", "settings.paths"}, {"page", 3},
                  {"keywords", "folders directories games saves profiles modules screenshots cache"}},
      QVariantMap{{"id", "runtime"}, {"name", "Runtime"}, {"feature", "settings.runtime"}, {"page", 4},
                  {"keywords", "cpu renderer offline runtime"}},
      QVariantMap{{"id", "graphics"}, {"name", "Graphics"}, {"feature", "settings.graphics"}, {"page", 5},
                  {"keywords", "vulkan d3d12 renderer graphics shader cache"}},
      QVariantMap{{"id", "input"}, {"name", "Input"}, {"feature", "settings.input"}, {"page", 6},
                  {"keywords", "controller keyboard gamepad input rumble deadzone xinput sdl hotas flight stick background"}},
      QVariantMap{{"id", "audio"}, {"name", "Audio"}, {"feature", "settings.audio"}, {"page", 7},
                  {"keywords", "sound audio volume device latency mute"}},
      QVariantMap{{"id", "network"}, {"name", "Network"}, {"feature", "settings.network"}, {"page", 8},
                  {"keywords", "network online multiplayer"}},
      QVariantMap{{"id", "filesystem"}, {"name", "Filesystem"}, {"feature", "settings.filesystem"}, {"page", 9},
                  {"keywords", "filesystem vfs mounts devices symbolic links paths"}},
      QVariantMap{{"id", "updates"}, {"name", "Updates"}, {"feature", "settings.updates"}, {"page", 10},
                  {"keywords", "updates modules catalog releases prerelease automatic interval"}},
      QVariantMap{{"id", "community"}, {"name", "Community"}, {"feature", "settings.community"}, {"page", 11},
                  {"keywords", "discord community support help rich presence social powered by xenon"}},
      QVariantMap{{"id", "accessibility"}, {"name", "Accessibility"}, {"feature", "settings.accessibility"}, {"page", 12},
                  {"keywords", "text size scale contrast motion keyboard focus accessibility"}},
      QVariantMap{{"id", "developer"}, {"name", "Developer"}, {"feature", "settings.developer"}, {"page", 13},
                  {"keywords", "test fixture diagnostics logging gracemeria development"}},
      QVariantMap{{"id", "about"}, {"name", "About"}, {"feature", "settings.about"}, {"page", 14},
                  {"keywords", "version system qt github licence diagnostics discord community"}},
      QVariantMap{{"id", "system"}, {"name", "Window & System"}, {"feature", "settings.system"}, {"page", 15},
                  {"keywords", "window position size startup minimized single instance protocol xenon links windows desktop system"}},
  };
}

QVariantMap SettingsCatalog::definition(const QString& key) {
  const auto* definition = findDefinition(key);
  if (definition == nullptr) return {};
  QVariantMap result{{QStringLiteral("key"), QString::fromLatin1(definition->key)},
                     {QStringLiteral("category"), QString::fromLatin1(definition->category)},
                     {QStringLiteral("type"), QString::fromLatin1(definition->type)},
                     {QStringLiteral("default"), definition->default_value},
                     {QStringLiteral("options"), definition->options},
                     {QStringLiteral("requiresRestart"), definition->requires_restart}};
  if (definition->has_range) {
    result.insert(QStringLiteral("minimum"), definition->minimum);
    result.insert(QStringLiteral("maximum"), definition->maximum);
  }
  return result;
}

QVariant SettingsCatalog::defaultValue(const QString& key) {
  const auto* definition = findDefinition(key);
  return definition != nullptr ? definition->default_value : QVariant{};
}

QVariantList SettingsCatalog::options(const QString& key) {
  const auto* definition = findDefinition(key);
  return definition != nullptr ? definition->options : QVariantList{};
}

bool SettingsCatalog::contains(const QString& key) { return findDefinition(key) != nullptr; }

QString SettingsCatalog::categoryFor(const QString& key) {
  const auto* definition = findDefinition(key);
  return definition != nullptr ? QString::fromLatin1(definition->category) : QString{};
}

QStringList SettingsCatalog::keysForCategory(const QString& category_id) {
  QStringList result;
  const auto normalized = category_id.trimmed();
  for (const auto& definition : definitions()) {
    if (normalized == QString::fromLatin1(definition.category)) result.push_back(QString::fromLatin1(definition.key));
  }
  return result;
}

ServiceResult SettingsCatalog::normalize(const QString& key, const QVariant& value) {
  const auto* definition = findDefinition(key);
  if (definition == nullptr) {
    return ServiceResult::failure(QStringLiteral("Unknown setting"),
                                  QStringLiteral("The launcher does not recognise the setting '%1'.").arg(key));
  }

  const auto type = QString::fromLatin1(definition->type);
  QVariant normalized;
  if (type == QStringLiteral("bool")) {
    bool ok = false;
    normalized = normalizeBool(value, &ok);
    if (!ok) {
      return ServiceResult::failure(QStringLiteral("Invalid setting value"),
                                    QStringLiteral("'%1' expects an on/off value.").arg(key));
    }
  } else if (type == QStringLiteral("number")) {
    bool ok = false;
    auto number = value.toDouble(&ok);
    if (!ok) {
      return ServiceResult::failure(QStringLiteral("Invalid setting value"),
                                    QStringLiteral("'%1' expects a numeric value.").arg(key));
    }
    if (definition->has_range) number = qBound(definition->minimum, number, definition->maximum);
    normalized = number;
  } else if (type == QStringLiteral("enum")) {
    bool found = false;
    for (const auto& entry : definition->options) {
      const auto candidate = entry.toMap().value(QStringLiteral("value"));
      if (variantEquivalent(candidate, value)) {
        normalized = candidate;
        found = true;
        break;
      }
    }
    if (!found) {
      return ServiceResult::failure(QStringLiteral("Invalid setting value"),
                                    QStringLiteral("'%1' does not accept '%2'.").arg(key, value.toString()));
    }
  } else {
    normalized = value.toString().trimmed();
  }

  return ServiceResult::success({}, {}, normalized);
}

}  // namespace xenon::launcher::frontend_backend
