#pragma once

#include "../../../services/service_result.hpp"
#include "../../../services/settings_service.hpp"
#include "../settings_feature.hpp"

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace xenon::launcher::frontend_backend {

class AppearanceFeature final : public QObject {
  Q_OBJECT

 public:
  AppearanceFeature(SettingsService& storage, SettingsFeature& settings, QObject* parent = nullptr);

  [[nodiscard]] QString themeId() const;
  [[nodiscard]] QString accentId() const;
  [[nodiscard]] QString cornerStyle() const;
  [[nodiscard]] QString customAccentColor() const;
  [[nodiscard]] QString effectiveThemeId(bool system_dark) const;

  [[nodiscard]] ServiceResult setThemeId(const QString& value);
  [[nodiscard]] ServiceResult setAccentId(const QString& value);
  [[nodiscard]] ServiceResult setCornerStyle(const QString& value);
  [[nodiscard]] ServiceResult setCustomAccentColor(const QString& value);

  [[nodiscard]] QVariantList themes() const;
  [[nodiscard]] QVariantList accents() const;
  [[nodiscard]] QVariantList cornerStyles() const;
  [[nodiscard]] QVariantMap themeDefinition(const QString& theme_id) const;
  [[nodiscard]] QVariantMap accentDefinition(const QString& accent_id) const;
  [[nodiscard]] QVariantList backgroundVariants(const QString& theme_id) const;
  [[nodiscard]] QString backgroundVariant(const QString& theme_id) const;
  [[nodiscard]] QString backgroundAsset(const QString& theme_id, const QString& variant_id) const;
  [[nodiscard]] ServiceResult setBackgroundVariant(const QString& theme_id, const QString& variant_id);
  [[nodiscard]] ServiceResult resetAppearance();

 signals:
  void themeChanged();
  void accentChanged();
  void cornerStyleChanged();
  void customAccentChanged();
  void appearanceChanged();

 private:
  [[nodiscard]] static QString normalizeTheme(QString value);
  [[nodiscard]] static QString resolvedThemeId(QString value);
  [[nodiscard]] static QVariantMap buildAccent(const QString& id, const QString& label, const QString& color);
  [[nodiscard]] static QString canonicalColor(const QString& color, bool* ok = nullptr);
  [[nodiscard]] QString defaultVariant(const QString& theme_id) const;

  SettingsService& storage_;
  SettingsFeature& settings_;
};

}  // namespace xenon::launcher::frontend_backend
