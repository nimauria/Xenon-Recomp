#include "appearance_feature.hpp"

#include <QColor>
#include <QSet>
#include <QtMath>

namespace xenon::launcher::frontend_backend {
namespace {

QVariantMap palette(const QString& id, const QString& name, bool dark,
                    const QString& window, const QString& header, const QString& sidebar,
                    const QString& surface, const QString& surface_alt, const QString& surface_hover,
                    const QString& surface_raised, const QString& border, const QString& divider,
                    const QString& text, const QString& text_muted, const QString& accent,
                    const QString& accent_strong, const QString& accent_soft, const QString& accent_text,
                    const QString& success, const QString& warning, const QString& danger,
                    const QString& input, const QString& overlay, const QString& decor_family,
                    const QString& default_backdrop) {
  return {{"id", id}, {"name", name}, {"dark", dark}, {"window", window}, {"header", header},
          {"sidebar", sidebar}, {"surface", surface}, {"surfaceAlt", surface_alt},
          {"surfaceHover", surface_hover}, {"surfaceRaised", surface_raised}, {"border", border},
          {"divider", divider}, {"text", text}, {"textMuted", text_muted}, {"accent", accent},
          {"accentStrong", accent_strong}, {"accentSoft", accent_soft}, {"accentText", accent_text},
          {"success", success}, {"warning", warning}, {"danger", danger}, {"input", input},
          {"overlay", overlay}, {"decorFamily", decor_family}, {"defaultBackdrop", default_backdrop}};
}

QVariantMap background(const QString& id, const QString& label, const QString& asset) {
  return {{"id", id}, {"label", label}, {"asset", asset}};
}

QColor blend(const QColor& foreground, const QColor& background, double foreground_weight) {
  const auto w = qBound(0.0, foreground_weight, 1.0);
  return QColor::fromRgbF(foreground.redF() * w + background.redF() * (1.0 - w),
                          foreground.greenF() * w + background.greenF() * (1.0 - w),
                          foreground.blueF() * w + background.blueF() * (1.0 - w));
}

QString hex(const QColor& color) { return color.name(QColor::HexRgb).toUpper(); }

double relativeLuminance(const QColor& color) {
  const auto channel = [](double value) {
    return value <= 0.04045 ? value / 12.92 : qPow((value + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) + 0.0722 * channel(color.blueF());
}

}  // namespace

AppearanceFeature::AppearanceFeature(SettingsService& storage, SettingsFeature& settings, QObject* parent)
    : QObject(parent), storage_(storage), settings_(settings) {}

QString AppearanceFeature::normalizeTheme(QString value) {
  if (value == QStringLiteral("xenon-cyan")) value = QStringLiteral("xenon-dark");
  if (value == QStringLiteral("xenon-green") || value == QStringLiteral("graphite")) value = QStringLiteral("carbon");
  if (value == QStringLiteral("industrial-amber")) value = QStringLiteral("industrial");
  value = value.trimmed().toLower();
  static const QSet<QString> valid{QStringLiteral("system"), QStringLiteral("xenon-dark"), QStringLiteral("carbon"),
                                   QStringLiteral("industrial"), QStringLiteral("light")};
  return valid.contains(value) ? value : QStringLiteral("system");
}

QString AppearanceFeature::resolvedThemeId(QString value) {
  value = normalizeTheme(value);
  return value == QStringLiteral("system") ? QStringLiteral("xenon-dark") : value;
}

QString AppearanceFeature::canonicalColor(const QString& color, bool* ok) {
  QColor parsed(color.trimmed());
  const auto valid = parsed.isValid();
  if (ok != nullptr) *ok = valid;
  return valid ? hex(parsed) : QString{};
}

QString AppearanceFeature::themeId() const {
  return normalizeTheme(storage_.stringValue(QStringLiteral("ui/theme"), QStringLiteral("system")));
}

QString AppearanceFeature::accentId() const {
  const auto value = storage_.stringValue(QStringLiteral("ui/accent"), QStringLiteral("default")).trimmed().toLower();
  static const QSet<QString> valid{QStringLiteral("default"), QStringLiteral("cyan"), QStringLiteral("emerald"),
                                   QStringLiteral("amber"), QStringLiteral("violet"), QStringLiteral("rose"),
                                   QStringLiteral("custom")};
  return valid.contains(value) ? value : QStringLiteral("default");
}

QString AppearanceFeature::cornerStyle() const {
  const auto value = storage_.stringValue(QStringLiteral("ui/corners"), QStringLiteral("rounded")).trimmed().toLower();
  return (value == QStringLiteral("rounded") || value == QStringLiteral("soft") || value == QStringLiteral("square"))
             ? value
             : QStringLiteral("rounded");
}

QString AppearanceFeature::customAccentColor() const {
  bool ok = false;
  const auto color = canonicalColor(storage_.stringValue(QStringLiteral("ui/customAccent"), QStringLiteral("#35D7EA")), &ok);
  return ok ? color : QStringLiteral("#35D7EA");
}

QString AppearanceFeature::effectiveThemeId(bool system_dark) const {
  const auto selected = themeId();
  if (selected == QStringLiteral("system")) {
    return system_dark ? QStringLiteral("xenon-dark") : QStringLiteral("light");
  }
  return resolvedThemeId(selected);
}

ServiceResult AppearanceFeature::setThemeId(const QString& value) {
  const auto normalized = normalizeTheme(value);
  if (normalized == themeId()) return ServiceResult::success();
  storage_.setValue(QStringLiteral("ui/theme"), normalized);
  emit themeChanged();
  emit appearanceChanged();
  return ServiceResult::success();
}

ServiceResult AppearanceFeature::setAccentId(const QString& value) {
  const auto normalized = value.trimmed().toLower();
  const auto valid = accents();
  bool found = false;
  for (const auto& entry : valid) {
    if (entry.toMap().value(QStringLiteral("id")).toString() == normalized) {
      found = true;
      break;
    }
  }
  if (!found) {
    return ServiceResult::failure(QStringLiteral("Invalid accent"),
                                  QStringLiteral("'%1' is not a registered Xenon accent.").arg(value));
  }
  if (normalized == accentId()) return ServiceResult::success();
  storage_.setValue(QStringLiteral("ui/accent"), normalized);
  emit accentChanged();
  emit appearanceChanged();
  return ServiceResult::success();
}

ServiceResult AppearanceFeature::setCornerStyle(const QString& value) {
  const auto normalized = value.trimmed().toLower();
  if (normalized != QStringLiteral("rounded") && normalized != QStringLiteral("soft") && normalized != QStringLiteral("square")) {
    return ServiceResult::failure(QStringLiteral("Invalid corner style"),
                                  QStringLiteral("The requested control-corner style is not supported."));
  }
  if (normalized == cornerStyle()) return ServiceResult::success();
  storage_.setValue(QStringLiteral("ui/corners"), normalized);
  emit cornerStyleChanged();
  emit appearanceChanged();
  return ServiceResult::success();
}

ServiceResult AppearanceFeature::setCustomAccentColor(const QString& value) {
  bool ok = false;
  const auto normalized = canonicalColor(value, &ok);
  if (!ok) {
    return ServiceResult::failure(QStringLiteral("Invalid accent colour"),
                                  QStringLiteral("Use a valid colour such as #35D7EA."));
  }
  if (normalized == customAccentColor()) return ServiceResult::success();
  storage_.setValue(QStringLiteral("ui/customAccent"), normalized);
  emit customAccentChanged();
  emit appearanceChanged();
  return ServiceResult::success();
}

QVariantList AppearanceFeature::themes() const {
  return {
      QVariantMap{{"id", "system"}, {"label", "System"}, {"description", "Follow the operating-system light/dark preference."}},
      QVariantMap{{"id", "xenon-dark"}, {"label", "Xenon Dark"}, {"description", "Deep navy surfaces with cyan Xenon instrumentation."}},
      QVariantMap{{"id", "carbon"}, {"label", "Carbon"}, {"description", "Neutral graphite surfaces with restrained green technical accents."}},
      QVariantMap{{"id", "industrial"}, {"label", "Industrial"}, {"description", "Warm dark-metal surfaces with amber instrumentation."}},
      QVariantMap{{"id", "light"}, {"label", "Light"}, {"description", "Bright neutral surfaces with a minimal technical backdrop."}},
  };
}

QVariantList AppearanceFeature::accents() const {
  return {
      QVariantMap{{"id", "default"}, {"label", "Theme default"}, {"color", ""}},
      QVariantMap{{"id", "cyan"}, {"label", "Cyan"}, {"color", "#35D7EA"}},
      QVariantMap{{"id", "emerald"}, {"label", "Emerald"}, {"color", "#45D995"}},
      QVariantMap{{"id", "amber"}, {"label", "Amber"}, {"color", "#EFB23B"}},
      QVariantMap{{"id", "violet"}, {"label", "Violet"}, {"color", "#A78BFA"}},
      QVariantMap{{"id", "rose"}, {"label", "Rose"}, {"color", "#F2799A"}},
      QVariantMap{{"id", "custom"}, {"label", "Custom"}, {"color", customAccentColor()}},
  };
}

QVariantList AppearanceFeature::cornerStyles() const {
  return {QVariantMap{{"id", "rounded"}, {"label", "Rounded"}},
          QVariantMap{{"id", "soft"}, {"label", "Soft"}},
          QVariantMap{{"id", "square"}, {"label", "Square"}}};
}

QVariantMap AppearanceFeature::themeDefinition(const QString& theme_id) const {
  const auto id = resolvedThemeId(theme_id);
  if (id == QStringLiteral("carbon")) {
    return palette(id, QStringLiteral("Carbon"), true, "#0D1114", "#11161A", "#101519", "#171D21", "#1D252A",
                   "#263137", "#1B2227", "#3B4A52", "#2E3A40", "#F4F6F7", "#B1BCC2", "#7ED6C6", "#54BFAE",
                   "#203A36", "#07110F", "#55D690", "#E7B54B", "#EF6970", "#131A1E", "#D006090B", "green", "nebula");
  }
  if (id == QStringLiteral("industrial")) {
    return palette(id, QStringLiteral("Industrial"), true, "#141513", "#1A1C19", "#181A17", "#20231F", "#292D27",
                   "#333830", "#252923", "#5A5442", "#454136", "#F2F0E9", "#B6B1A6", "#EFB23B", "#D79620",
                   "#49391F", "#171006", "#62D58A", "#EFB23B", "#F06A60", "#1B1D1A", "#D00A0B0A", "amber", "orbit");
  }
  if (id == QStringLiteral("light")) {
    return palette(id, QStringLiteral("Light"), false, "#F4F7F9", "#FFFFFF", "#F8FAFB", "#FFFFFF", "#F0F4F6",
                   "#E8F0F3", "#FFFFFF", "#CAD8DF", "#DEE7EB", "#142230", "#60707C", "#18C8BA", "#0AA99D",
                   "#D9F5F1", "#071B18", "#159B61", "#B87E0F", "#D64654", "#FBFCFD", "#330B1B2A", "neutral", "minimal");
  }
  return palette(QStringLiteral("xenon-dark"), QStringLiteral("Xenon Dark"), true, "#07131D", "#081822", "#091923",
                 "#0D202C", "#122936", "#173544", "#112633", "#294757", "#213945", "#F4F8FA", "#A9BAC4",
                 "#35D7EA", "#15B8CF", "#133943", "#031014", "#45D995", "#E7B54B", "#F06470", "#0A1B26",
                 "#CC020910", "blue", "orbit");
}

QVariantMap AppearanceFeature::buildAccent(const QString& id, const QString& label, const QString& color) {
  QColor base(color);
  if (!base.isValid()) return {};
  const auto strong = base.darker(122);
  const auto soft_dark = blend(base, QColor(QStringLiteral("#09141B")), 0.24);
  const auto soft_light = blend(base, QColor(QStringLiteral("#FFFFFF")), 0.18);
  const auto text = relativeLuminance(base) > 0.42 ? QColor(QStringLiteral("#061014")) : QColor(QStringLiteral("#FFFFFF"));
  return {{"id", id}, {"label", label}, {"accent", hex(base)}, {"strong", hex(strong)},
          {"softDark", hex(soft_dark)}, {"softLight", hex(soft_light)}, {"text", hex(text)}};
}

QVariantMap AppearanceFeature::accentDefinition(const QString& accent_id) const {
  const auto id = accent_id.trimmed().toLower();
  if (id == QStringLiteral("default")) return {{"id", "default"}, {"usesCustom", false}};
  if (id == QStringLiteral("cyan")) return buildAccent(id, QStringLiteral("Cyan"), QStringLiteral("#35D7EA"));
  if (id == QStringLiteral("emerald")) return buildAccent(id, QStringLiteral("Emerald"), QStringLiteral("#45D995"));
  if (id == QStringLiteral("amber")) return buildAccent(id, QStringLiteral("Amber"), QStringLiteral("#EFB23B"));
  if (id == QStringLiteral("violet")) return buildAccent(id, QStringLiteral("Violet"), QStringLiteral("#A78BFA"));
  if (id == QStringLiteral("rose")) return buildAccent(id, QStringLiteral("Rose"), QStringLiteral("#F2799A"));
  if (id == QStringLiteral("custom")) return buildAccent(id, QStringLiteral("Custom"), customAccentColor());
  return {{"id", "default"}, {"usesCustom", false}};
}

QVariantList AppearanceFeature::backgroundVariants(const QString& theme_id) const {
  const auto id = resolvedThemeId(theme_id);
  if (id == QStringLiteral("xenon-dark")) {
    return {background("orbit", "Orbit", "qrc:/theme-art/backgrounds/theme-xenon-dark-orbit.png"),
            background("tech", "Tech", "qrc:/theme-art/backgrounds/theme-xenon-dark-tech.png"),
            background("hud", "HUD", "qrc:/theme-art/backgrounds/theme-xenon-dark-hud.png")};
  }
  if (id == QStringLiteral("carbon")) {
    return {background("nebula", "Nebula", "qrc:/theme-art/backgrounds/theme-carbon-nebula.png"),
            background("tech", "Tech", "qrc:/theme-art/backgrounds/theme-carbon-tech.png")};
  }
  if (id == QStringLiteral("industrial")) {
    return {background("orbit", "Orbit", "qrc:/theme-art/backgrounds/theme-industrial-orbit.png"),
            background("tech", "Tech", "qrc:/theme-art/backgrounds/theme-industrial-tech.png")};
  }
  return {background("minimal", "Minimal", "")};
}

QString AppearanceFeature::defaultVariant(const QString& theme_id) const {
  const auto variants = backgroundVariants(theme_id);
  return variants.isEmpty() ? QStringLiteral("minimal")
                            : variants.first().toMap().value(QStringLiteral("id"), QStringLiteral("minimal")).toString();
}

QString AppearanceFeature::backgroundVariant(const QString& theme_id) const {
  const auto id = resolvedThemeId(theme_id);
  const auto stored = settings_.stringValue(QStringLiteral("appearance/backdropVariant/") + id, defaultVariant(id));
  const auto variants = backgroundVariants(id);
  for (const auto& entry : variants) {
    if (entry.toMap().value(QStringLiteral("id")).toString() == stored) return stored;
  }
  return defaultVariant(id);
}

QString AppearanceFeature::backgroundAsset(const QString& theme_id, const QString& variant_id) const {
  const auto variants = backgroundVariants(theme_id);
  for (const auto& entry : variants) {
    const auto map = entry.toMap();
    if (map.value(QStringLiteral("id")).toString() == variant_id) return map.value(QStringLiteral("asset")).toString();
  }
  const auto fallback = defaultVariant(theme_id);
  for (const auto& entry : variants) {
    const auto map = entry.toMap();
    if (map.value(QStringLiteral("id")).toString() == fallback) return map.value(QStringLiteral("asset")).toString();
  }
  return {};
}

ServiceResult AppearanceFeature::setBackgroundVariant(const QString& theme_id, const QString& variant_id) {
  const auto id = resolvedThemeId(theme_id);
  for (const auto& entry : backgroundVariants(id)) {
    if (entry.toMap().value(QStringLiteral("id")).toString() == variant_id) {
      settings_.setValue(QStringLiteral("appearance/backdropVariant/") + id, variant_id);
      emit appearanceChanged();
      return ServiceResult::success();
    }
  }
  return ServiceResult::failure(QStringLiteral("Invalid theme background"),
                                QStringLiteral("That background does not belong to the selected Xenon theme."));
}

ServiceResult AppearanceFeature::resetAppearance() {
  storage_.remove(QStringLiteral("ui/theme"));
  storage_.remove(QStringLiteral("ui/accent"));
  storage_.remove(QStringLiteral("ui/corners"));
  storage_.remove(QStringLiteral("ui/customAccent"));
  for (const auto& key : {QStringLiteral("appearance/themeBackdrop"), QStringLiteral("appearance/backdropIntensity"),
                          QStringLiteral("appearance/artworkBackgrounds"), QStringLiteral("appearance/decorLevel"),
                          QStringLiteral("appearance/panelOpacity"), QStringLiteral("general/compact")}) {
    settings_.reset(key);
  }
  for (const auto& theme : {QStringLiteral("xenon-dark"), QStringLiteral("carbon"), QStringLiteral("industrial"),
                            QStringLiteral("light")}) {
    settings_.reset(QStringLiteral("appearance/backdropVariant/") + theme);
  }
  emit themeChanged();
  emit accentChanged();
  emit cornerStyleChanged();
  emit customAccentChanged();
  emit appearanceChanged();
  return ServiceResult::success(QStringLiteral("Appearance reset"),
                                QStringLiteral("Theme and appearance preferences were restored to their defaults."));
}

}  // namespace xenon::launcher::frontend_backend
