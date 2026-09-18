#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QColor>

class LauncherBridge final : public QObject {
  Q_OBJECT

  Q_PROPERTY(QString version READ version CONSTANT)
  Q_PROPERTY(bool backendConnected READ backendConnected CONSTANT)
  Q_PROPERTY(bool testMode READ testMode CONSTANT)
  Q_PROPERTY(QString themeId READ themeId WRITE setThemeId NOTIFY themeIdChanged)
  Q_PROPERTY(QString accentId READ accentId WRITE setAccentId NOTIFY accentIdChanged)
  Q_PROPERTY(QString cornerStyle READ cornerStyle WRITE setCornerStyle NOTIFY cornerStyleChanged)
  Q_PROPERTY(QString profileName READ profileName WRITE setProfileName NOTIFY profileNameChanged)
  Q_PROPERTY(bool systemDark READ systemDark NOTIFY systemAppearanceChanged)
  Q_PROPERTY(bool systemHighContrast READ systemHighContrast NOTIFY systemAppearanceChanged)

  // Host facts are detected by the launcher. They are deliberately read-only:
  // users should not be offered impossible host architectures or platforms.
  Q_PROPERTY(QString hostArchitecture READ hostArchitecture CONSTANT)
  Q_PROPERTY(QString platformName READ platformName CONSTANT)
  Q_PROPERTY(QString qtVersion READ qtVersion CONSTANT)
  Q_PROPERTY(QStringList availableGraphicsBackends READ availableGraphicsBackends CONSTANT)
  Q_PROPERTY(QString appDataPath READ appDataPath CONSTANT)
  Q_PROPERTY(QString configPath READ configPath CONSTANT)
  Q_PROPERTY(QString defaultGameLibraryPath READ defaultGameLibraryPath CONSTANT)
  Q_PROPERTY(QString defaultSaveDataPath READ defaultSaveDataPath CONSTANT)
  Q_PROPERTY(QString defaultProfilesPath READ defaultProfilesPath CONSTANT)
  Q_PROPERTY(QString defaultModulesPath READ defaultModulesPath CONSTANT)
  Q_PROPERTY(QString defaultScreenshotsPath READ defaultScreenshotsPath CONSTANT)
  Q_PROPERTY(QString cachePath READ cachePath CONSTANT)

 public:
  explicit LauncherBridge(QObject* parent = nullptr);

  [[nodiscard]] QString version() const;
  [[nodiscard]] bool backendConnected() const noexcept;
  [[nodiscard]] bool testMode() const noexcept;

  [[nodiscard]] QString themeId() const;
  void setThemeId(const QString& theme_id);

  [[nodiscard]] QString accentId() const;
  void setAccentId(const QString& accent_id);

  [[nodiscard]] QString cornerStyle() const;
  void setCornerStyle(const QString& corner_style);

  [[nodiscard]] QString profileName() const;
  void setProfileName(const QString& profile_name);

  [[nodiscard]] bool systemDark() const noexcept;
  [[nodiscard]] bool systemHighContrast() const noexcept;

  [[nodiscard]] QString hostArchitecture() const;
  [[nodiscard]] QString platformName() const;
  [[nodiscard]] QString qtVersion() const;
  [[nodiscard]] QStringList availableGraphicsBackends() const;
  [[nodiscard]] QString appDataPath() const;
  [[nodiscard]] QString configPath() const;
  [[nodiscard]] QString defaultGameLibraryPath() const;
  [[nodiscard]] QString defaultSaveDataPath() const;
  [[nodiscard]] QString defaultProfilesPath() const;
  [[nodiscard]] QString defaultModulesPath() const;
  [[nodiscard]] QString defaultScreenshotsPath() const;
  [[nodiscard]] QString cachePath() const;

  Q_INVOKABLE void notifyUnavailable(const QString& feature);
  Q_INVOKABLE void notify(const QString& title, const QString& message);

  // Stable frontend seams for the future update/module services. QML calls
  // these operations instead of knowing repository URLs or download logic.
  Q_INVOKABLE void requestLauncherUpdateCheck();
  Q_INVOKABLE void requestModuleCatalogRefresh();
  Q_INVOKABLE void requestModuleUpdate(const QString& module_id);
  Q_INVOKABLE void requestModuleInstall(const QString& module_id);
  Q_INVOKABLE void rememberPage(int page_index);
  Q_INVOKABLE int rememberedPage() const;

  // Generic front-end preference storage. Runtime-backed services can replace
  // individual keys later while keeping the QML control contract stable.
  Q_INVOKABLE QVariant settingValue(const QString& key,
                                    const QVariant& fallback = {}) const;
  Q_INVOKABLE bool boolSetting(const QString& key, bool fallback = false) const;
  Q_INVOKABLE QString stringSetting(const QString& key,
                                    const QString& fallback = {}) const;
  Q_INVOKABLE double numberSetting(const QString& key, double fallback = 0.0) const;
  Q_INVOKABLE int intSetting(const QString& key, int fallback = 0) const;
  Q_INVOKABLE void setSettingValue(const QString& key, const QVariant& value);
  Q_INVOKABLE void resetSetting(const QString& key);

  // Compile-time / runtime feature gate seam used by dynamic settings pages.
  Q_INVOKABLE bool featureEnabled(const QString& feature) const noexcept;

  Q_INVOKABLE QString loadProfileState() const;
  Q_INVOKABLE bool saveProfileState(const QString& json);

  // Converts FileDialog / FolderDialog URLs to readable native paths.
  Q_INVOKABLE QString toLocalPath(const QUrl& url) const;

  Q_INVOKABLE bool openFolder(const QString& path);
  Q_INVOKABLE bool openExternalUrl(const QString& url) const;
  Q_INVOKABLE void copyDiagnostics() const;
  Q_INVOKABLE QString developerDiagnostics() const;
  Q_INVOKABLE QString userDiagnostics() const;
  Q_INVOKABLE void copyText(const QString& text) const;

  // Theme-aware branding. Branding SVGs use currentColor; this helper
  // substitutes the active theme colour before exposing them to QML.
  Q_INVOKABLE QString themedBrandingDataUrl(const QString& asset_name,
                                             const QString& color) const;

  // Profile avatar storage stays local to the configured profiles directory.
  Q_INVOKABLE QString importProfileAvatar(const QString& profile_id,
                                           const QUrl& source_url);
  Q_INVOKABLE bool removeProfileAvatar(const QString& profile_id);

 signals:
  void themeIdChanged();
  void accentIdChanged();
  void cornerStyleChanged();
  void profileNameChanged();
  void systemAppearanceChanged();
  void notificationRequested(const QString& title, const QString& message);
  void settingChanged(const QString& key, const QVariant& value);

 private:
  QString theme_id_;
  QString accent_id_;
  QString corner_style_;
  QString profile_name_;
};
