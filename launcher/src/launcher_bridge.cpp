#include "launcher_bridge.hpp"

#include "frontend_backend/frontend_backend.hpp"
#include "launcher_config.hpp"
#include "services/service_result.hpp"

#include <QCoreApplication>
#include <QDebug>

#ifndef XENON_LAUNCHER_VERSION
#define XENON_LAUNCHER_VERSION "0.0.0-dev"
#endif

using xenon::launcher::ServiceResult;
using xenon::launcher::frontend_backend::FrontendBackend;

LauncherBridge::LauncherBridge(QObject* parent)
    : QObject(parent), backend_(std::make_unique<FrontendBackend>()) {
  auto& application = backend_->application();
  auto& settings = backend_->settings();
  auto& profiles = backend_->profiles();
  auto& library = backend_->library();
  auto& dlc = backend_->dlc();
  auto& modules = backend_->modules();
  auto& updates = backend_->updates();
  auto& session = backend_->session();
  auto& community = backend_->community();

  auto& appearance = backend_->appearance();
  connect(&appearance, &xenon::launcher::frontend_backend::AppearanceFeature::themeChanged,
          this, &LauncherBridge::themeIdChanged);
  connect(&appearance, &xenon::launcher::frontend_backend::AppearanceFeature::accentChanged,
          this, &LauncherBridge::accentIdChanged);
  connect(&appearance, &xenon::launcher::frontend_backend::AppearanceFeature::cornerStyleChanged,
          this, &LauncherBridge::cornerStyleChanged);
  connect(&appearance, &xenon::launcher::frontend_backend::AppearanceFeature::customAccentChanged,
          this, &LauncherBridge::customAccentColorChanged);
  connect(&application, &xenon::launcher::frontend_backend::ApplicationFeature::systemAppearanceChanged,
          this, &LauncherBridge::systemAppearanceChanged);
  connect(&settings, &xenon::launcher::frontend_backend::SettingsFeature::changed,
          this, &LauncherBridge::settingChanged);
  connect(&profiles, &xenon::launcher::frontend_backend::ProfilesFeature::changed, this, [this]() {
    emit profileNameChanged();
    emit profilesChanged();
  });
  connect(&library, &xenon::launcher::frontend_backend::LibraryFeature::changed,
          this, &LauncherBridge::libraryChanged);
  connect(&dlc, &xenon::launcher::frontend_backend::DlcFeature::changed, this,
          [this](const QString& game_id) { emit libraryDlcChanged(game_id); });
  connect(&modules, &xenon::launcher::frontend_backend::ModulesFeature::changed,
          this, &LauncherBridge::modulesChanged);
  connect(&modules, &xenon::launcher::frontend_backend::ModulesFeature::catalogChanged,
          this, &LauncherBridge::moduleCatalogChanged);
  connect(&modules, &xenon::launcher::frontend_backend::ModulesFeature::updateStateChanged,
          this, &LauncherBridge::moduleUpdateStateChanged);
  connect(&modules, &xenon::launcher::frontend_backend::ModulesFeature::notificationRequested,
          this, &LauncherBridge::notificationRequested);
  connect(&updates, &xenon::launcher::frontend_backend::UpdateFeature::changed,
          this, &LauncherBridge::updateStateChanged);
  connect(&updates, &xenon::launcher::frontend_backend::UpdateFeature::notificationRequested,
          this, &LauncherBridge::notificationRequested);
  connect(&updates, &xenon::launcher::frontend_backend::UpdateFeature::restartRequested,
          this, []() { QCoreApplication::quit(); }, Qt::QueuedConnection);
  connect(&session, &xenon::launcher::frontend_backend::SessionController::changed,
          this, &LauncherBridge::sessionChanged);
  connect(&session, &xenon::launcher::frontend_backend::SessionController::historyChanged,
          this, &LauncherBridge::sessionHistoryChanged);
  connect(&session, &xenon::launcher::frontend_backend::SessionController::notificationRequested,
          this, &LauncherBridge::notificationRequested);
  connect(&community, &xenon::launcher::frontend_backend::CommunityFeature::changed,
          this, &LauncherBridge::communityChanged);

  const auto initialization = backend_->initialize();
  if (!initialization.ok) {
    qWarning().noquote() << "Xenon launcher frontend backend initialization failed:"
                         << initialization.title << '-' << initialization.message;
  }
}

LauncherBridge::~LauncherBridge() = default;

QString LauncherBridge::version() const { return QStringLiteral(XENON_LAUNCHER_VERSION); }
bool LauncherBridge::backendConnected() const noexcept { return backend_->runtime().connected(); }
QString LauncherBridge::runtimeStatus() const { return backend_->runtime().status(); }
bool LauncherBridge::testMode() const noexcept { return xenon::launcher::kTestMode; }
QVariantMap LauncherBridge::runtimeCapabilities() const { return backend_->runtime().capabilities(); }
bool LauncherBridge::runtimeCapability(const QString& capability) const {
  return backend_->runtime().capability(capability);
}

QString LauncherBridge::themeId() const { return backend_->appearance().themeId(); }
void LauncherBridge::setThemeId(const QString& theme_id) {
  const auto result = backend_->appearance().setThemeId(theme_id);
  if (!result.ok) notifyResult(result);
}
QString LauncherBridge::accentId() const { return backend_->appearance().accentId(); }
void LauncherBridge::setAccentId(const QString& accent_id) {
  const auto result = backend_->appearance().setAccentId(accent_id);
  if (!result.ok) notifyResult(result);
}
QString LauncherBridge::cornerStyle() const { return backend_->appearance().cornerStyle(); }
void LauncherBridge::setCornerStyle(const QString& corner_style) {
  const auto result = backend_->appearance().setCornerStyle(corner_style);
  if (!result.ok) notifyResult(result);
}
QString LauncherBridge::customAccentColor() const { return backend_->appearance().customAccentColor(); }
void LauncherBridge::setCustomAccentColor(const QString& color) {
  const auto result = backend_->appearance().setCustomAccentColor(color);
  if (!result.ok) notifyResult(result);
}

QString LauncherBridge::profileName() const {
  return backend_->profiles().activeProfile().value(QStringLiteral("profileName"), QStringLiteral("Nimauria")).toString();
}

void LauncherBridge::setProfileName(const QString& profile_name) {
  const auto trimmed = profile_name.trimmed();
  if (trimmed.isEmpty() || trimmed == this->profileName()) return;
  const auto index = backend_->profiles().activeIndex();
  auto data = backend_->profiles().activeProfile();
  data.insert(QStringLiteral("profileName"), trimmed);
  const auto result = backend_->profiles().update(index, data);
  if (!result.ok) notifyResult(result, false);
}

int LauncherBridge::profileNameLimit() const noexcept { return backend_->profiles().profileNameLimit(); }
int LauncherBridge::profileDescriptionLimit() const noexcept { return backend_->profiles().descriptionLimit(); }

bool LauncherBridge::systemDark() const noexcept { return backend_->application().systemDark(); }
bool LauncherBridge::systemHighContrast() const noexcept { return backend_->application().systemHighContrast(); }
QString LauncherBridge::hostArchitecture() const { return backend_->diagnostics().hostArchitecture(); }
QString LauncherBridge::platformName() const { return backend_->diagnostics().platformName(); }
QString LauncherBridge::qtVersion() const { return backend_->diagnostics().qtVersion(); }
QStringList LauncherBridge::availableGraphicsBackends() const {
  return backend_->runtime().availableGraphicsBackends(testMode());
}

QString LauncherBridge::appDataPath() const { return backend_->paths().appDataPath(); }
QString LauncherBridge::configPath() const { return backend_->paths().configPath(); }
QString LauncherBridge::defaultGameLibraryPath() const { return backend_->paths().defaultGameLibraryPath(); }
QString LauncherBridge::defaultSaveDataPath() const { return backend_->paths().defaultSaveDataPath(); }
QString LauncherBridge::defaultProfilesPath() const { return backend_->paths().defaultProfilesPath(); }
QString LauncherBridge::defaultModulesPath() const { return backend_->paths().defaultModulesPath(); }
QString LauncherBridge::defaultScreenshotsPath() const { return backend_->paths().defaultScreenshotsPath(); }
QString LauncherBridge::cachePath() const { return backend_->paths().cachePath(); }

void LauncherBridge::notifyUnavailable(const QString& feature) {
  emit notificationRequested(feature,
      QStringLiteral("This feature is owned by the launcher frontend backend, but its external Xenon/provider operation is not connected yet."));
}

void LauncherBridge::notify(const QString& title, const QString& message) {
  emit notificationRequested(title, message);
}

void LauncherBridge::notifyResult(const ServiceResult& result, bool notify_success) {
  if (result.title.isEmpty() && result.message.isEmpty()) return;
  if (result.ok && !notify_success) return;
  emit notificationRequested(result.title.isEmpty() ? QStringLiteral("Xenon Launcher") : result.title,
                             result.message);
}

void LauncherBridge::requestLauncherUpdateCheck() {
  const auto result = backend_->updates().checkLauncher(true);
  if (!result.ok) notifyResult(result);
}

void LauncherBridge::requestLauncherUpdateDownload() {
  const auto result = backend_->updates().downloadLauncher();
  if (!result.ok) notifyResult(result);
}

void LauncherBridge::requestLauncherUpdateInstall() {
  const auto result = backend_->updates().installLauncher();
  if (!result.ok) notifyResult(result);
}

void LauncherBridge::cancelLauncherUpdateDownload() {
  notifyResult(backend_->updates().cancelLauncherDownload());
}

QVariantMap LauncherBridge::launcherUpdateState() const { return backend_->updates().launcherState(); }

void LauncherBridge::requestModuleCatalogRefresh() {
  notifyResult(backend_->modules().refreshCatalog());
}

QVariantList LauncherBridge::moduleCatalogEntries() const { return backend_->modules().catalogEntries(); }
QVariantMap LauncherBridge::moduleCatalogState() const { return backend_->modules().catalogState(); }
QVariantMap LauncherBridge::moduleUpdateState(const QString& module_id) const {
  return backend_->modules().updateState(module_id);
}

void LauncherBridge::requestModuleUpdate(const QString& module_id) {
  notifyResult(backend_->modules().checkForUpdate(module_id));
}
void LauncherBridge::requestModuleUpdateCheck(const QString& module_id) {
  notifyResult(backend_->modules().checkForUpdate(module_id));
}
void LauncherBridge::requestModuleUpdateDownload(const QString& module_id) {
  notifyResult(backend_->modules().downloadUpdate(module_id));
}
void LauncherBridge::requestModuleUpdateInstall(const QString& module_id) {
  notifyResult(backend_->modules().installUpdate(module_id));
}
void LauncherBridge::cancelModuleUpdateDownload(const QString& module_id) {
  notifyResult(backend_->modules().cancelUpdateDownload(module_id));
}
void LauncherBridge::requestAllModuleUpdateChecks() {
  notifyResult(backend_->modules().checkAllUpdates());
}
void LauncherBridge::requestModuleInstall(const QString& module_id) {
  notifyResult(backend_->modules().requestInstall(module_id));
}

void LauncherBridge::rememberPage(int page_index) { backend_->application().rememberPage(page_index); }
int LauncherBridge::rememberedPage() const { return backend_->application().rememberedPage(); }
int LauncherBridge::initialPage() const { return backend_->initialPage(); }
QString LauncherBridge::effectiveThemeId() const {
  return backend_->appearance().effectiveThemeId(backend_->application().systemDark());
}

QVariant LauncherBridge::settingValue(const QString& key, const QVariant& fallback) const {
  return backend_->settings().value(key, fallback);
}
bool LauncherBridge::boolSetting(const QString& key, bool fallback) const {
  return backend_->settings().boolValue(key, fallback);
}
QString LauncherBridge::stringSetting(const QString& key, const QString& fallback) const {
  return backend_->settings().stringValue(key, fallback);
}
double LauncherBridge::numberSetting(const QString& key, double fallback) const {
  return backend_->settings().numberValue(key, fallback);
}
int LauncherBridge::intSetting(const QString& key, int fallback) const {
  return backend_->settings().intValue(key, fallback);
}
void LauncherBridge::setSettingValue(const QString& key, const QVariant& value) {
  const auto result = backend_->setSettingValue(key, value);
  if (!result.ok) notifyResult(result);
}
void LauncherBridge::resetSetting(const QString& key) {
  const auto result = backend_->resetSetting(key);
  if (!result.ok) notifyResult(result);
}
QVariantList LauncherBridge::settingsCategories() const { return backend_->settings().categories(); }
QVariantMap LauncherBridge::settingDefinition(const QString& key) const { return backend_->settings().definition(key); }
QVariant LauncherBridge::settingDefaultValue(const QString& key) const { return backend_->settings().defaultValue(key); }
QVariantList LauncherBridge::settingOptions(const QString& key) const { return backend_->settings().options(key); }
void LauncherBridge::resetSettingsCategory(const QString& category_id) {
  notifyResult(backend_->resetSettingsCategory(category_id));
}
void LauncherBridge::resetAllSettings() { notifyResult(backend_->resetAllSettings()); }

QVariantList LauncherBridge::themeCatalog() const { return backend_->appearance().themes(); }
QVariantList LauncherBridge::accentCatalog() const { return backend_->appearance().accents(); }
QVariantList LauncherBridge::cornerStyleCatalog() const { return backend_->appearance().cornerStyles(); }
QVariantMap LauncherBridge::themeDefinition(const QString& theme_id) const {
  return backend_->appearance().themeDefinition(theme_id);
}
QVariantMap LauncherBridge::accentDefinition(const QString& accent_id) const {
  return backend_->appearance().accentDefinition(accent_id);
}
QVariantList LauncherBridge::themeBackgroundVariants(const QString& theme_id) const {
  return backend_->appearance().backgroundVariants(theme_id);
}
QString LauncherBridge::themeBackgroundVariant(const QString& theme_id) const {
  return backend_->appearance().backgroundVariant(theme_id);
}
QString LauncherBridge::themeBackgroundAsset(const QString& theme_id, const QString& variant_id) const {
  return backend_->appearance().backgroundAsset(theme_id, variant_id);
}

bool LauncherBridge::featureEnabled(const QString& feature) const noexcept {
  return backend_->application().featureEnabled(feature);
}

QVariantList LauncherBridge::profileEntries() const { return backend_->profiles().entries(); }
QVariantList LauncherBridge::profileActions(int index) const { return backend_->profiles().actions(index); }
QVariantList LauncherBridge::profileRuntimeDefinitions() const { return backend_->profiles().runtimeDefinitions(); }
QVariantMap LauncherBridge::profileRuntimeDefaults() const { return backend_->profiles().runtimeDefaults(); }
int LauncherBridge::activeProfileIndex() const { return backend_->profiles().activeIndex(); }
bool LauncherBridge::profileNameAvailable(const QString& name, int exclude_index) const {
  return backend_->profiles().nameAvailable(name, exclude_index);
}
int LauncherBridge::createProfile(const QVariantMap& data) {
  const auto result = backend_->profiles().create(data);
  if (!result.ok) notifyResult(result);
  return result.ok ? result.data.toInt() : -1;
}
bool LauncherBridge::updateProfile(int index, const QVariantMap& data) {
  const auto result = backend_->profiles().update(index, data);
  if (!result.ok) notifyResult(result);
  return result.ok;
}
int LauncherBridge::duplicateProfile(int index) {
  const auto result = backend_->profiles().duplicate(index);
  notifyResult(result);
  return result.ok ? result.data.toInt() : -1;
}
bool LauncherBridge::activateProfile(int index) {
  const auto result = backend_->profiles().activate(index);
  if (!result.ok) notifyResult(result);
  return result.ok;
}
bool LauncherBridge::removeProfile(int index) {
  const auto result = backend_->profiles().remove(index);
  if (!result.ok) notifyResult(result);
  return result.ok;
}
bool LauncherBridge::openProfileStorage(int index) {
  const auto prepared = backend_->profiles().ensureStorage(index);
  if (!prepared.ok) {
    notifyResult(prepared);
    return false;
  }
  const auto result = backend_->filesystem().openFolder(prepared.data.toString());
  if (!result.ok) notifyResult(result);
  return result.ok;
}
bool LauncherBridge::removeProfileImage(int index) {
  const auto result = backend_->profiles().removeAvatarForProfile(index);
  notifyResult(result);
  return result.ok;
}
bool LauncherBridge::exportProfile(int index, const QUrl& destination) {
  const auto result = backend_->importExport().exportProfile(index, destination);
  notifyResult(result);
  return result.ok;
}
int LauncherBridge::importProfile(const QUrl& source) {
  const auto result = backend_->importExport().importProfile(source);
  notifyResult(result);
  return result.ok ? result.data.toInt() : -1;
}

QVariantList LauncherBridge::libraryEntries() const { return backend_->library().entries(); }
QVariantList LauncherBridge::libraryDlcEntries(const QString& game_id) const {
  return backend_->dlc().entries(game_id);
}
QVariantMap LauncherBridge::libraryGameProperties(const QString& game_id) const {
  return backend_->gameProperties().properties(game_id);
}
bool LauncherBridge::importGameContent(const QList<QUrl>& sources) {
  const auto result = backend_->importExport().importGameContent(sources);
  notifyResult(result);
  return result.ok;
}
bool LauncherBridge::importDlcContent(const QString& game_id, const QList<QUrl>& sources) {
  const auto result = backend_->importExport().importDlc(game_id, sources);
  notifyResult(result);
  return result.ok;
}

bool LauncherBridge::importDlcContentForEntry(const QString& game_id, const QString& dlc_id,
                                              const QList<QUrl>& sources) {
  const auto result = backend_->importExport().importDlc(game_id, sources, dlc_id);
  notifyResult(result);
  return result.ok;
}
bool LauncherBridge::removeLibraryEntry(const QString& game_id) {
  const auto result = backend_->library().remove(game_id);
  notifyResult(result);
  return result.ok;
}
bool LauncherBridge::verifyLibraryEntry(const QString& game_id) {
  const auto result = backend_->library().verify(game_id);
  notifyResult(result);
  return result.ok;
}
QString LauncherBridge::libraryContentPath(const QString& game_id) const {
  return backend_->library().contentPath(game_id);
}
QString LauncherBridge::libraryContentFolder(const QString& game_id) const {
  return backend_->library().contentFolder(game_id);
}
QString LauncherBridge::libraryManagedFolder(const QString& game_id) const {
  return backend_->library().managedPath(game_id);
}
QString LauncherBridge::libraryDlcFolder(const QString& game_id) const {
  return backend_->dlc().rootPath(game_id);
}
QString LauncherBridge::libraryDlcItemFolder(const QString& game_id, const QString& dlc_id) const {
  return backend_->dlc().itemPath(game_id, dlc_id);
}
bool LauncherBridge::verifyLibraryDlc(const QString& game_id, const QString& dlc_id) {
  const auto result = backend_->dlc().verify(game_id, dlc_id);
  notifyResult(result);
  return result.ok;
}
bool LauncherBridge::removeLibraryDlc(const QString& game_id, const QString& dlc_id) {
  const auto result = backend_->dlc().remove(game_id, dlc_id);
  notifyResult(result);
  return result.ok;
}
QVariantMap LauncherBridge::launchConfiguration(const QString& game_id) const {
  return backend_->launch().configurationFor(game_id);
}
bool LauncherBridge::launchGame(const QString& game_id) {
  const auto result = backend_->session().start(game_id);
  if (!result.ok) notifyResult(result);
  return result.ok;
}
bool LauncherBridge::stopGame() {
  const auto result = backend_->session().stop();
  if (!result.ok) notifyResult(result);
  return result.ok;
}
void LauncherBridge::dismissSessionFailure() { backend_->session().dismissFailure(); }
QVariantMap LauncherBridge::currentSession() const { return backend_->session().currentSession(); }
QVariantList LauncherBridge::sessionHistory() const { return backend_->session().history(); }
QString LauncherBridge::sessionState() const { return backend_->session().state(); }
void LauncherBridge::clearSessionHistory() { backend_->session().clearHistory(); }

QVariantMap LauncherBridge::communityInfo() const { return backend_->community().info(); }
QVariantMap LauncherBridge::discordPresenceState() const { return backend_->community().discordPresenceState(); }
bool LauncherBridge::openDiscordCommunity() {
  const auto result = backend_->community().openDiscord();
  if (!result.ok) notifyResult(result);
  return result.ok;
}
bool LauncherBridge::openProjectCommunity() {
  const auto result = backend_->community().openProjectPage();
  if (!result.ok) notifyResult(result);
  return result.ok;
}
void LauncherBridge::setCommunityPage(const QString& page_name) { backend_->community().setPage(page_name); }
void LauncherBridge::refreshDiscordPresence() { (void)backend_->community().refreshDiscordPresence(); }

QVariantList LauncherBridge::moduleEntries() const { return backend_->modules().entries(); }
QVariantList LauncherBridge::moduleActions(const QString& module_id) const {
  return backend_->modules().actions(module_id);
}
QVariantList LauncherBridge::modulePageActions() const { return backend_->modules().pageActions(); }
bool LauncherBridge::refreshModules() {
  const auto result = backend_->modules().refresh();
  notifyResult(result);
  return result.ok;
}
bool LauncherBridge::importModulePackages(const QList<QUrl>& sources) {
  const auto result = backend_->importExport().importModulePackages(sources);
  notifyResult(result);
  return result.ok;
}
bool LauncherBridge::setModuleEnabled(const QString& module_id, bool enabled) {
  const auto result = backend_->modules().setEnabled(module_id, enabled);
  notifyResult(result);
  return result.ok;
}
bool LauncherBridge::removeModule(const QString& module_id) {
  const auto result = backend_->modules().remove(module_id);
  notifyResult(result);
  return result.ok;
}
bool LauncherBridge::verifyModule(const QString& module_id) {
  const auto result = backend_->modules().verify(module_id);
  notifyResult(result);
  return result.ok;
}
bool LauncherBridge::unlinkModuleGame(const QString& module_id) {
  const auto result = backend_->modules().unlinkGame(module_id);
  notifyResult(result);
  return result.ok;
}
QString LauncherBridge::modulePath(const QString& module_id) const {
  return backend_->modules().modulePath(module_id);
}
QVariantList LauncherBridge::moduleSettingsSchema(const QString& module_id) const {
  return backend_->modules().settingsSchema(module_id);
}
bool LauncherBridge::setModuleSetting(const QString& module_id, const QString& setting_id,
                                      const QVariant& value) {
  const auto result = backend_->modules().setSetting(module_id, setting_id, value);
  notifyResult(result);
  return result.ok;
}

QString LauncherBridge::toLocalPath(const QUrl& url) const { return backend_->filesystem().toLocalPath(url); }
bool LauncherBridge::canOpenPath(const QString& path) const { return backend_->filesystem().canOpenPath(path); }
bool LauncherBridge::openFolder(const QString& path) {
  const auto result = backend_->filesystem().openFolder(path);
  if (!result.ok) notifyResult(result);
  return result.ok;
}
bool LauncherBridge::openExternalUrl(const QString& url) {
  const auto result = backend_->filesystem().openExternalUrl(url);
  if (!result.ok) notifyResult(result);
  return result.ok;
}
QString LauncherBridge::developerDiagnostics() const { return backend_->diagnostics().developerDiagnostics(); }
QString LauncherBridge::userDiagnostics() const { return backend_->diagnostics().userDiagnostics(); }
void LauncherBridge::copyText(const QString& text) {
  const auto result = backend_->filesystem().copyText(text);
  if (!result.ok) notifyResult(result);
}
void LauncherBridge::copyDiagnostics() { copyText(developerDiagnostics()); }
QString LauncherBridge::themedBrandingDataUrl(const QString& asset_name, const QString& color) const {
  return backend_->branding().themedDataUrl(asset_name, color);
}
QString LauncherBridge::importProfileAvatar(const QString& profile_id, const QUrl& source_url) {
  const auto result = backend_->importExport().importProfileAvatar(profile_id, source_url);
  if (!result.ok) {
    notifyResult(result);
    return {};
  }
  return result.data.toString();
}
bool LauncherBridge::removeProfileAvatar(const QString& profile_id) {
  return backend_->importExport().removeProfileAvatar(profile_id);
}
