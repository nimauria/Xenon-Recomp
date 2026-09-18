#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

#include "services/service_result.hpp"

namespace xenon::launcher::frontend_backend {
class FrontendBackend;
}

class LauncherBridge final : public QObject {
  Q_OBJECT

  Q_PROPERTY(QString version READ version CONSTANT)
  Q_PROPERTY(bool backendConnected READ backendConnected NOTIFY backendConnectedChanged)
  Q_PROPERTY(QString runtimeStatus READ runtimeStatus NOTIFY backendConnectedChanged)
  Q_PROPERTY(bool testMode READ testMode CONSTANT)
  Q_PROPERTY(QString themeId READ themeId WRITE setThemeId NOTIFY themeIdChanged)
  Q_PROPERTY(QString accentId READ accentId WRITE setAccentId NOTIFY accentIdChanged)
  Q_PROPERTY(QString cornerStyle READ cornerStyle WRITE setCornerStyle NOTIFY cornerStyleChanged)
  Q_PROPERTY(QString customAccentColor READ customAccentColor WRITE setCustomAccentColor NOTIFY customAccentColorChanged)
  Q_PROPERTY(QString profileName READ profileName WRITE setProfileName NOTIFY profileNameChanged)
  Q_PROPERTY(int profileNameLimit READ profileNameLimit CONSTANT)
  Q_PROPERTY(int profileDescriptionLimit READ profileDescriptionLimit CONSTANT)
  Q_PROPERTY(bool systemDark READ systemDark NOTIFY systemAppearanceChanged)
  Q_PROPERTY(bool systemHighContrast READ systemHighContrast NOTIFY systemAppearanceChanged)
  Q_PROPERTY(QVariantMap currentSession READ currentSession NOTIFY sessionChanged)
  Q_PROPERTY(QString sessionState READ sessionState NOTIFY sessionChanged)
  Q_PROPERTY(QVariantMap communityInfo READ communityInfo CONSTANT)
  Q_PROPERTY(QVariantMap discordPresenceState READ discordPresenceState NOTIFY communityChanged)

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
  ~LauncherBridge() override;

  [[nodiscard]] QString version() const;
  [[nodiscard]] bool backendConnected() const noexcept;
  [[nodiscard]] QString runtimeStatus() const;
  [[nodiscard]] bool testMode() const noexcept;
  Q_INVOKABLE QVariantMap runtimeCapabilities() const;
  Q_INVOKABLE bool runtimeCapability(const QString& capability) const;

  [[nodiscard]] QString themeId() const;
  void setThemeId(const QString& theme_id);
  [[nodiscard]] QString accentId() const;
  void setAccentId(const QString& accent_id);
  [[nodiscard]] QString cornerStyle() const;
  void setCornerStyle(const QString& corner_style);
  [[nodiscard]] QString customAccentColor() const;
  void setCustomAccentColor(const QString& color);
  [[nodiscard]] QString profileName() const;
  void setProfileName(const QString& profile_name);
  [[nodiscard]] int profileNameLimit() const noexcept;
  [[nodiscard]] int profileDescriptionLimit() const noexcept;

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

  Q_INVOKABLE void requestLauncherUpdateCheck();
  Q_INVOKABLE void requestLauncherUpdateDownload();
  Q_INVOKABLE void requestLauncherUpdateInstall();
  Q_INVOKABLE void cancelLauncherUpdateDownload();
  Q_INVOKABLE QVariantMap launcherUpdateState() const;
  Q_INVOKABLE void requestModuleCatalogRefresh();
  Q_INVOKABLE QVariantList moduleCatalogEntries() const;
  Q_INVOKABLE QVariantMap moduleCatalogState() const;
  Q_INVOKABLE QVariantMap moduleUpdateState(const QString& module_id) const;
  Q_INVOKABLE void requestModuleUpdate(const QString& module_id);
  Q_INVOKABLE void requestModuleUpdateCheck(const QString& module_id);
  Q_INVOKABLE void requestModuleUpdateDownload(const QString& module_id);
  Q_INVOKABLE void requestModuleUpdateInstall(const QString& module_id);
  Q_INVOKABLE void cancelModuleUpdateDownload(const QString& module_id);
  Q_INVOKABLE void requestAllModuleUpdateChecks();
  Q_INVOKABLE void requestModuleInstall(const QString& module_id);
  Q_INVOKABLE void rememberPage(int page_index);
  Q_INVOKABLE int rememberedPage() const;
  Q_INVOKABLE int initialPage() const;
  Q_INVOKABLE QString effectiveThemeId() const;

  Q_INVOKABLE QVariant settingValue(const QString& key, const QVariant& fallback = {}) const;
  Q_INVOKABLE bool boolSetting(const QString& key, bool fallback = false) const;
  Q_INVOKABLE QString stringSetting(const QString& key, const QString& fallback = {}) const;
  Q_INVOKABLE double numberSetting(const QString& key, double fallback = 0.0) const;
  Q_INVOKABLE int intSetting(const QString& key, int fallback = 0) const;
  Q_INVOKABLE void setSettingValue(const QString& key, const QVariant& value);
  Q_INVOKABLE void resetSetting(const QString& key);
  Q_INVOKABLE QVariantList settingsCategories() const;
  Q_INVOKABLE QVariantMap settingDefinition(const QString& key) const;
  Q_INVOKABLE QVariant settingDefaultValue(const QString& key) const;
  Q_INVOKABLE QVariantList settingOptions(const QString& key) const;
  Q_INVOKABLE void resetSettingsCategory(const QString& category_id);
  Q_INVOKABLE void resetAllSettings();

  Q_INVOKABLE QVariantList themeCatalog() const;
  Q_INVOKABLE QVariantList accentCatalog() const;
  Q_INVOKABLE QVariantList cornerStyleCatalog() const;
  Q_INVOKABLE QVariantMap themeDefinition(const QString& theme_id) const;
  Q_INVOKABLE QVariantMap accentDefinition(const QString& accent_id) const;
  Q_INVOKABLE QVariantList themeBackgroundVariants(const QString& theme_id) const;
  Q_INVOKABLE QString themeBackgroundVariant(const QString& theme_id) const;
  Q_INVOKABLE QString themeBackgroundAsset(const QString& theme_id, const QString& variant_id) const;
  Q_INVOKABLE bool featureEnabled(const QString& feature) const noexcept;

  Q_INVOKABLE QVariantList profileEntries() const;
  Q_INVOKABLE QVariantList profileActions(int index) const;
  Q_INVOKABLE QVariantList profileRuntimeDefinitions() const;
  Q_INVOKABLE QVariantMap profileRuntimeDefaults() const;
  Q_INVOKABLE int activeProfileIndex() const;
  Q_INVOKABLE bool profileNameAvailable(const QString& name, int exclude_index = -1) const;
  Q_INVOKABLE int createProfile(const QVariantMap& data);
  Q_INVOKABLE bool updateProfile(int index, const QVariantMap& data);
  Q_INVOKABLE int duplicateProfile(int index);
  Q_INVOKABLE bool activateProfile(int index);
  Q_INVOKABLE bool removeProfile(int index);
  Q_INVOKABLE bool openProfileStorage(int index);
  Q_INVOKABLE bool removeProfileImage(int index);
  Q_INVOKABLE bool exportProfile(int index, const QUrl& destination);
  Q_INVOKABLE int importProfile(const QUrl& source);

  Q_INVOKABLE QVariantList libraryEntries() const;
  Q_INVOKABLE QVariantList libraryDlcEntries(const QString& game_id) const;
  Q_INVOKABLE QVariantMap libraryGameProperties(const QString& game_id) const;
  Q_INVOKABLE bool importGameContent(const QList<QUrl>& sources);
  Q_INVOKABLE bool importDlcContent(const QString& game_id, const QList<QUrl>& sources);
  Q_INVOKABLE bool importDlcContentForEntry(const QString& game_id, const QString& dlc_id,
                                            const QList<QUrl>& sources);
  Q_INVOKABLE bool removeLibraryEntry(const QString& game_id);
  Q_INVOKABLE bool verifyLibraryEntry(const QString& game_id);
  Q_INVOKABLE QString libraryContentPath(const QString& game_id) const;
  Q_INVOKABLE QString libraryContentFolder(const QString& game_id) const;
  Q_INVOKABLE QString libraryManagedFolder(const QString& game_id) const;
  Q_INVOKABLE QString libraryDlcFolder(const QString& game_id) const;
  Q_INVOKABLE QString libraryDlcItemFolder(const QString& game_id, const QString& dlc_id) const;
  Q_INVOKABLE bool verifyLibraryDlc(const QString& game_id, const QString& dlc_id);
  Q_INVOKABLE bool removeLibraryDlc(const QString& game_id, const QString& dlc_id);
  Q_INVOKABLE QVariantMap launchConfiguration(const QString& game_id) const;
  Q_INVOKABLE bool launchGame(const QString& game_id);
  Q_INVOKABLE bool stopGame();
  Q_INVOKABLE void dismissSessionFailure();
  Q_INVOKABLE QVariantMap currentSession() const;
  Q_INVOKABLE QVariantList sessionHistory() const;
  Q_INVOKABLE QString sessionState() const;
  Q_INVOKABLE void clearSessionHistory();

  Q_INVOKABLE QVariantMap communityInfo() const;
  Q_INVOKABLE QVariantMap discordPresenceState() const;
  Q_INVOKABLE bool openDiscordCommunity();
  Q_INVOKABLE bool openProjectCommunity();
  Q_INVOKABLE void setCommunityPage(const QString& page_name);
  Q_INVOKABLE void refreshDiscordPresence();

  Q_INVOKABLE QVariantList moduleEntries() const;
  Q_INVOKABLE QVariantList moduleActions(const QString& module_id) const;
  Q_INVOKABLE QVariantList modulePageActions() const;
  Q_INVOKABLE bool refreshModules();
  Q_INVOKABLE bool importModulePackages(const QList<QUrl>& sources);
  Q_INVOKABLE bool setModuleEnabled(const QString& module_id, bool enabled);
  Q_INVOKABLE bool removeModule(const QString& module_id);
  Q_INVOKABLE bool verifyModule(const QString& module_id);
  Q_INVOKABLE bool unlinkModuleGame(const QString& module_id);
  Q_INVOKABLE QString modulePath(const QString& module_id) const;
  Q_INVOKABLE QVariantList moduleSettingsSchema(const QString& module_id) const;
  Q_INVOKABLE bool setModuleSetting(const QString& module_id, const QString& setting_id,
                                    const QVariant& value);

  Q_INVOKABLE QString toLocalPath(const QUrl& url) const;
  Q_INVOKABLE bool canOpenPath(const QString& path) const;
  Q_INVOKABLE bool openFolder(const QString& path);
  Q_INVOKABLE bool openExternalUrl(const QString& url);
  Q_INVOKABLE void copyDiagnostics();
  Q_INVOKABLE QString developerDiagnostics() const;
  Q_INVOKABLE QString userDiagnostics() const;
  Q_INVOKABLE void copyText(const QString& text);
  Q_INVOKABLE QString themedBrandingDataUrl(const QString& asset_name, const QString& color) const;
  Q_INVOKABLE QString importProfileAvatar(const QString& profile_id, const QUrl& source_url);
  Q_INVOKABLE bool removeProfileAvatar(const QString& profile_id);

 signals:
  void themeIdChanged();
  void accentIdChanged();
  void cornerStyleChanged();
  void customAccentColorChanged();
  void profileNameChanged();
  void systemAppearanceChanged();
  void backendConnectedChanged();
  void profilesChanged();
  void libraryChanged();
  void libraryDlcChanged(const QString& game_id);
  void sessionChanged();
  void sessionHistoryChanged();
  void communityChanged();
  void modulesChanged();
  void updateStateChanged();
  void moduleCatalogChanged();
  void moduleUpdateStateChanged(const QString& module_id);
  void notificationRequested(const QString& title, const QString& message);
  void settingChanged(const QString& key, const QVariant& value);

 private:
  void notifyResult(const xenon::launcher::ServiceResult& result, bool notify_success = true);

  std::unique_ptr<xenon::launcher::frontend_backend::FrontendBackend> backend_;
};
