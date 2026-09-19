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

namespace {
QString notificationSeverityFor(const QString& title, const QString& message) {
  const auto text = (title + QLatin1Char(' ') + message).toLower();
  if (text.contains(QStringLiteral("fail")) || text.contains(QStringLiteral("error")) ||
      text.contains(QStringLiteral("invalid")) || text.contains(QStringLiteral("unavailable")) ||
      text.contains(QStringLiteral("could not"))) {
    return QStringLiteral("error");
  }
  if (text.contains(QStringLiteral("warning")) || text.contains(QStringLiteral("cancel")) ||
      text.contains(QStringLiteral("disabled")) || text.contains(QStringLiteral("safe mode"))) {
    return QStringLiteral("warning");
  }
  if (text.contains(QStringLiteral("installed")) || text.contains(QStringLiteral("ready")) ||
      text.contains(QStringLiteral("up to date")) || text.contains(QStringLiteral("rolled back")) ||
      text.contains(QStringLiteral("created")) || text.contains(QStringLiteral("imported")) ||
      text.contains(QStringLiteral("exported")) || text.contains(QStringLiteral("reset"))) {
    return QStringLiteral("success");
  }
  return QStringLiteral("info");
}
}

LauncherBridge::LauncherBridge(QObject* parent)
    : QObject(parent), backend_(std::make_unique<FrontendBackend>()) {
  auto& application = backend_->application();
  auto& recovery = backend_->recovery();
  auto& settings = backend_->settings();
  auto& profiles = backend_->profiles();
  auto& library = backend_->library();
  auto& dlc = backend_->dlc();
  auto& modules = backend_->modules();
  auto& updates = backend_->updates();
  auto& session = backend_->session();
  auto& community = backend_->community();
  auto& command_palette = backend_->commandPalette();
  auto& notifications = backend_->notifications();
  auto& home = backend_->home();
  auto& input = backend_->input();

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
  connect(&recovery, &xenon::launcher::frontend_backend::RecoveryFeature::changed,
          this, &LauncherBridge::recoveryChanged);
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
  connect(&modules, &xenon::launcher::frontend_backend::ModulesFeature::updateHistoryChanged,
          this, &LauncherBridge::moduleUpdateHistoryChanged);
  connect(&modules, &xenon::launcher::frontend_backend::ModulesFeature::notificationRequested,
          this, [this](const QString& title, const QString& message) {
            pushNotification(title, message, notificationSeverityFor(title, message),
                             QStringLiteral("modules"), QStringLiteral("navigate.modules"), {}, {},
                             QStringLiteral("Open Modules"));
          });
  connect(&updates, &xenon::launcher::frontend_backend::UpdateFeature::changed,
          this, &LauncherBridge::updateStateChanged);
  connect(&updates, &xenon::launcher::frontend_backend::UpdateFeature::notificationRequested,
          this, [this](const QString& title, const QString& message) {
            pushNotification(title, message, notificationSeverityFor(title, message),
                             QStringLiteral("updates"), QStringLiteral("navigate.settings"),
                             QStringLiteral("updates"), {}, QStringLiteral("Open Updates"));
          });
  connect(&updates, &xenon::launcher::frontend_backend::UpdateFeature::restartRequested,
          this, []() { QCoreApplication::quit(); }, Qt::QueuedConnection);
  connect(&session, &xenon::launcher::frontend_backend::SessionController::changed,
          this, &LauncherBridge::sessionChanged);
  connect(&session, &xenon::launcher::frontend_backend::SessionController::historyChanged,
          this, &LauncherBridge::sessionHistoryChanged);
  connect(&session, &xenon::launcher::frontend_backend::SessionController::notificationRequested,
          this, [this](const QString& title, const QString& message) {
            const auto game_id = backend_->session().currentSession().value(QStringLiteral("gameId")).toString();
            pushNotification(title, message, notificationSeverityFor(title, message),
                             QStringLiteral("session"),
                             game_id.isEmpty() ? QStringLiteral("navigate.library") : QStringLiteral("navigate.game"),
                             game_id, {}, QStringLiteral("Open Library"));
          });
  connect(&community, &xenon::launcher::frontend_backend::CommunityFeature::changed,
          this, &LauncherBridge::communityChanged);
  connect(&command_palette, &xenon::launcher::frontend_backend::CommandPaletteFeature::changed,
          this, &LauncherBridge::commandPaletteChanged);
  connect(&command_palette, &xenon::launcher::frontend_backend::CommandPaletteFeature::navigationRequested,
          this, &LauncherBridge::navigationRequested);

  connect(&notifications, &xenon::launcher::frontend_backend::NotificationCenterFeature::changed,
          this, &LauncherBridge::notificationsChanged);
  connect(&home, &xenon::launcher::frontend_backend::HomeFeature::changed,
          this, &LauncherBridge::homeChanged);

  connect(&input, &xenon::launcher::frontend_backend::InputFeature::changed,
          this, &LauncherBridge::inputChanged);

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
bool LauncherBridge::safeMode() const noexcept { return backend_->recovery().safeMode(); }
QVariantMap LauncherBridge::recoveryState() const { return backend_->recovery().state(); }
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

void LauncherBridge::pushNotification(const QString& title, const QString& message,
                                      const QString& severity, const QString& source,
                                      const QString& command_id, const QString& target_id,
                                      const QString& section_id, const QString& action_label,
                                      bool show_toast) {
  if (title.trimmed().isEmpty() && message.trimmed().isEmpty()) return;
  (void)backend_->notifications().add(title, message, severity, source, command_id, target_id,
                                      section_id, action_label);
  if (show_toast) {
    emit notificationRequested(title.trimmed().isEmpty() ? QStringLiteral("Xenon Launcher") : title, message);
  }
}

void LauncherBridge::notifyUnavailable(const QString& feature) {
  pushNotification(feature,
      QStringLiteral("This feature is owned by the launcher frontend backend, but its external Xenon/provider operation is not connected yet."),
      QStringLiteral("warning"));
}

void LauncherBridge::notify(const QString& title, const QString& message) {
  pushNotification(title, message, notificationSeverityFor(title, message));
}

int LauncherBridge::notificationUnreadCount() const noexcept {
  return backend_->notifications().unreadCount();
}

QVariantList LauncherBridge::notifications() const { return backend_->notifications().entries(); }
QVariantMap LauncherBridge::homeSnapshot() const { return backend_->home().snapshot(); }

void LauncherBridge::markNotificationRead(const QString& notification_id, bool read) {
  const auto result = backend_->notifications().markRead(notification_id, read);
  if (!result.ok) notifyResult(result, false);
}

void LauncherBridge::markAllNotificationsRead() {
  const auto result = backend_->notifications().markAllRead();
  if (!result.ok) notifyResult(result, false);
}

void LauncherBridge::dismissNotification(const QString& notification_id) {
  const auto result = backend_->notifications().dismiss(notification_id);
  if (!result.ok) notifyResult(result, false);
}

void LauncherBridge::clearNotifications() {
  const auto result = backend_->notifications().clear();
  if (!result.ok) notifyResult(result, false);
}

void LauncherBridge::executeNotificationAction(const QString& notification_id) {
  const auto notification = backend_->notifications().entry(notification_id);
  if (notification.isEmpty()) {
    notifyResult(ServiceResult::failure(QStringLiteral("Notifications"),
                                        QStringLiteral("That notification no longer exists.")));
    return;
  }
  (void)backend_->notifications().markRead(notification_id, true);
  const auto command = notification.value(QStringLiteral("commandId")).toString();
  if (command.isEmpty()) return;
  const auto result = backend_->commandPalette().execute(
      command, notification.value(QStringLiteral("targetId")).toString(),
      notification.value(QStringLiteral("sectionId")).toString());
  if (!result.ok) notifyResult(result);
}

QString LauncherBridge::recoveryDirectory() const { return backend_->recovery().recoveryDirectory(); }

void LauncherBridge::acknowledgeRecovery() {
  const auto result = backend_->recovery().acknowledge();
  if (!result.ok) notifyResult(result);
}

void LauncherBridge::markLauncherReady() {
  const auto result = backend_->recovery().markInteractive();
  if (!result.ok) notifyResult(result, false);
}

void LauncherBridge::restartInSafeMode() {
  const auto result = backend_->recovery().restartInSafeMode();
  if (!result.ok) notifyResult(result);
}

void LauncherBridge::restartNormally() {
  const auto result = backend_->recovery().restartNormally();
  if (!result.ok) notifyResult(result);
}

void LauncherBridge::notifyResult(const ServiceResult& result, bool notify_success,
                                  const QString& source, const QString& command_id,
                                  const QString& target_id, const QString& section_id,
                                  const QString& action_label) {
  if (result.title.isEmpty() && result.message.isEmpty()) return;
  if (result.ok && !notify_success) return;
  pushNotification(result.title.isEmpty() ? QStringLiteral("Xenon Launcher") : result.title,
                   result.message, result.ok ? QStringLiteral("success") : QStringLiteral("error"),
                   source, command_id, target_id, section_id, action_label);
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
QVariantList LauncherBridge::moduleUpdateHistory(const QString& module_id) const {
  return backend_->modules().updateHistory(module_id);
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
void LauncherBridge::requestModuleRollback(const QString& module_id) {
  notifyResult(backend_->modules().rollbackUpdate(module_id));
}
void LauncherBridge::clearModuleUpdateHistory(const QString& module_id) {
  notifyResult(backend_->modules().clearUpdateHistory(module_id));
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

QVariantList LauncherBridge::commandPaletteResults(const QString& query, int limit) const {
  return backend_->commandPalette().search(query, limit);
}

void LauncherBridge::executeCommandPaletteAction(const QString& command_id,
                                                 const QString& target_id,
                                                 const QString& section_id) {
  notifyResult(backend_->commandPalette().execute(command_id, target_id, section_id));
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

bool LauncherBridge::inputAvailable() const { return backend_->input().available(); }
QString LauncherBridge::inputStatus() const { return backend_->input().status(); }
QVariantList LauncherBridge::inputDevices() const { return backend_->input().devices(); }
QVariantList LauncherBridge::inputUsers() const { return backend_->input().users(); }
QVariantList LauncherBridge::inputProfiles() const { return backend_->input().profiles(); }
QVariantMap LauncherBridge::inputDiagnostics() const { return backend_->input().diagnostics(); }
QVariantMap LauncherBridge::inputModuleApiInfo() const { return backend_->input().moduleApiInfo(); }
QString LauncherBridge::inputProfileStorePath() const { return backend_->input().profileStorePath(); }
void LauncherBridge::refreshInputDevices() { notifyResult(backend_->input().refresh(), false); }
void LauncherBridge::reconfigureInput() { notifyResult(backend_->input().reconfigure()); }
void LauncherBridge::assignInputDevice(int user_index, const QString& identity_key) {
  notifyResult(backend_->input().assignUser(user_index, identity_key), false);
}
void LauncherBridge::clearInputDevice(int user_index) {
  notifyResult(backend_->input().clearUser(user_index), false);
}
void LauncherBridge::addInputSource(int user_index, const QString& identity_key) {
  notifyResult(backend_->input().addUserSource(user_index, identity_key), false);
}
void LauncherBridge::removeInputSource(int user_index, const QString& identity_key) {
  notifyResult(backend_->input().removeUserSource(user_index, identity_key), false);
}
void LauncherBridge::bindInputProfile(int user_index, const QString& profile_id) {
  notifyResult(backend_->input().bindUserProfile(user_index, profile_id), false);
}
void LauncherBridge::clearInputProfile(int user_index) {
  notifyResult(backend_->input().clearUserProfile(user_index), false);
}
void LauncherBridge::testInputVibration(int user_index) {
  notifyResult(backend_->input().testVibration(user_index));
}

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
QVariantList LauncherBridge::profileBackgroundActions() const { return backend_->profiles().backgroundActions(); }
QVariantList LauncherBridge::profileRuntimeDefinitions() const { return backend_->profiles().runtimeDefinitions(); }
QVariantMap LauncherBridge::profileRuntimeDefaults() const { return backend_->profiles().runtimeDefaults(); }
int LauncherBridge::activeProfileIndex() const { return backend_->profiles().activeIndex(); }
bool LauncherBridge::profileNameAvailable(const QString& name, int exclude_index) const {
  return backend_->profiles().nameAvailable(name, exclude_index);
}
int LauncherBridge::createProfile(const QVariantMap& data) {
  const auto result = backend_->profiles().create(data);
  if (result.ok) {
    pushNotification(result.title, result.message, QStringLiteral("success"), QStringLiteral("profiles"),
                     QStringLiteral("navigate.profiles"), {}, {}, QStringLiteral("Open Profiles"), false);
  } else {
    notifyResult(result, true, QStringLiteral("profiles"), QStringLiteral("navigate.profiles"), {}, {},
                 QStringLiteral("Open Profiles"));
  }
  return result.ok ? result.data.toInt() : -1;
}
bool LauncherBridge::updateProfile(int index, const QVariantMap& data) {
  const auto result = backend_->profiles().update(index, data);
  if (result.ok) {
    pushNotification(result.title, result.message, QStringLiteral("success"), QStringLiteral("profiles"),
                     QStringLiteral("navigate.profiles"), {}, {}, QStringLiteral("Open Profiles"), false);
  } else {
    notifyResult(result, true, QStringLiteral("profiles"), QStringLiteral("navigate.profiles"), {}, {},
                 QStringLiteral("Open Profiles"));
  }
  return result.ok;
}
int LauncherBridge::duplicateProfile(int index) {
  const auto result = backend_->profiles().duplicate(index);
  notifyResult(result, true, QStringLiteral("profiles"), QStringLiteral("navigate.profiles"), {}, {},
               QStringLiteral("Open Profiles"));
  return result.ok ? result.data.toInt() : -1;
}
bool LauncherBridge::activateProfile(int index) {
  const auto result = backend_->profiles().activate(index);
  if (result.ok) {
    pushNotification(result.title, result.message, QStringLiteral("success"), QStringLiteral("profiles"),
                     QStringLiteral("navigate.profiles"), {}, {}, QStringLiteral("Open Profiles"), false);
  } else {
    notifyResult(result, true, QStringLiteral("profiles"), QStringLiteral("navigate.profiles"), {}, {},
                 QStringLiteral("Open Profiles"));
  }
  return result.ok;
}
bool LauncherBridge::removeProfile(int index) {
  const auto result = backend_->profiles().remove(index);
  if (result.ok) {
    pushNotification(result.title, result.message, QStringLiteral("success"), QStringLiteral("profiles"),
                     QStringLiteral("navigate.profiles"), {}, {}, QStringLiteral("Open Profiles"), false);
  } else {
    notifyResult(result, true, QStringLiteral("profiles"), QStringLiteral("navigate.profiles"), {}, {},
                 QStringLiteral("Open Profiles"));
  }
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
  notifyResult(result, true, QStringLiteral("profiles"), QStringLiteral("navigate.profiles"), {}, {},
               QStringLiteral("Open Profiles"));
  return result.ok;
}
int LauncherBridge::importProfile(const QUrl& source) {
  const auto result = backend_->importExport().importProfile(source);
  notifyResult(result, true, QStringLiteral("profiles"), QStringLiteral("navigate.profiles"), {}, {},
               QStringLiteral("Open Profiles"));
  return result.ok ? result.data.toInt() : -1;
}

QVariantList LauncherBridge::libraryEntries() const { return backend_->library().entries(); }
QVariantList LauncherBridge::libraryGameActions(const QString& game_id) const {
  return backend_->library().actions(game_id);
}
QVariantList LauncherBridge::libraryManageActions(const QString& game_id) const {
  return backend_->library().manageActions(game_id);
}
QVariantList LauncherBridge::libraryBackgroundActions() const {
  return backend_->library().backgroundActions();
}
QVariantList LauncherBridge::libraryDlcEntries(const QString& game_id) const {
  return backend_->dlc().entries(game_id);
}
QVariantList LauncherBridge::libraryDlcActions(const QString& game_id, const QString& dlc_id) const {
  return backend_->dlc().actions(game_id, dlc_id);
}
QVariantList LauncherBridge::libraryDlcBackgroundActions(const QString& game_id) const {
  return backend_->dlc().backgroundActions(game_id);
}
QVariantMap LauncherBridge::libraryGameProperties(const QString& game_id) const {
  return backend_->gameProperties().properties(game_id);
}
bool LauncherBridge::importGameContent(const QList<QUrl>& sources) {
  const auto result = backend_->importExport().importGameContent(sources);
  notifyResult(result, true, QStringLiteral("library"), QStringLiteral("navigate.library"), {}, {},
               QStringLiteral("Open Library"));
  return result.ok;
}
bool LauncherBridge::importDlcContent(const QString& game_id, const QList<QUrl>& sources) {
  const auto result = backend_->importExport().importDlc(game_id, sources);
  notifyResult(result, true, QStringLiteral("library"), QStringLiteral("navigate.game"), game_id, {},
               QStringLiteral("Open Game"));
  return result.ok;
}

bool LauncherBridge::importDlcContentForEntry(const QString& game_id, const QString& dlc_id,
                                              const QList<QUrl>& sources) {
  const auto result = backend_->importExport().importDlc(game_id, sources, dlc_id);
  notifyResult(result, true, QStringLiteral("library"), QStringLiteral("navigate.game"), game_id, {},
               QStringLiteral("Open Game"));
  return result.ok;
}
bool LauncherBridge::removeLibraryEntry(const QString& game_id) {
  const auto result = backend_->library().remove(game_id);
  notifyResult(result, true, QStringLiteral("library"), QStringLiteral("navigate.library"), {}, {},
               QStringLiteral("Open Library"));
  return result.ok;
}
bool LauncherBridge::verifyLibraryEntry(const QString& game_id) {
  const auto result = backend_->library().verify(game_id);
  notifyResult(result, true, QStringLiteral("library"), QStringLiteral("navigate.game"), game_id, {},
               QStringLiteral("Open Game"));
  return result.ok;
}
bool LauncherBridge::refreshLibraryMetadata(const QString& game_id) {
  const auto result = backend_->library().refreshMetadata(game_id);
  notifyResult(result, true, QStringLiteral("library"), QStringLiteral("navigate.game"), game_id, {},
               QStringLiteral("Open Game"));
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
  notifyResult(result, true, QStringLiteral("library"), QStringLiteral("navigate.game"), game_id, {},
               QStringLiteral("Open Game"));
  return result.ok;
}
bool LauncherBridge::removeLibraryDlc(const QString& game_id, const QString& dlc_id) {
  const auto result = backend_->dlc().remove(game_id, dlc_id);
  notifyResult(result, true, QStringLiteral("library"), QStringLiteral("navigate.game"), game_id, {},
               QStringLiteral("Open Game"));
  return result.ok;
}
QVariantMap LauncherBridge::launchConfiguration(const QString& game_id) const {
  return backend_->launch().configurationFor(game_id);
}
bool LauncherBridge::launchGame(const QString& game_id) {
  if (backend_->recovery().safeMode()) {
    notifyResult(ServiceResult::failure(
        QStringLiteral("Safe Mode"),
        QStringLiteral("Game sessions are disabled in Safe Mode. Restart Xenon normally when you are ready to launch content.")));
    return false;
  }
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
  notifyResult(result, true, QStringLiteral("modules"), QStringLiteral("navigate.modules"), {}, {},
               QStringLiteral("Open Modules"));
  return result.ok;
}
bool LauncherBridge::importModulePackages(const QList<QUrl>& sources) {
  const auto result = backend_->importExport().importModulePackages(sources);
  notifyResult(result, true, QStringLiteral("modules"), QStringLiteral("navigate.modules"), {}, {},
               QStringLiteral("Open Modules"));
  return result.ok;
}
bool LauncherBridge::setModuleEnabled(const QString& module_id, bool enabled) {
  const auto result = backend_->modules().setEnabled(module_id, enabled);
  notifyResult(result, true, QStringLiteral("modules"), QStringLiteral("navigate.module"), module_id, {},
               QStringLiteral("Open Module"));
  return result.ok;
}
bool LauncherBridge::removeModule(const QString& module_id) {
  const auto result = backend_->modules().remove(module_id);
  notifyResult(result, true, QStringLiteral("modules"), QStringLiteral("navigate.module"), module_id, {},
               QStringLiteral("Open Module"));
  return result.ok;
}
bool LauncherBridge::verifyModule(const QString& module_id) {
  const auto result = backend_->modules().verify(module_id);
  notifyResult(result, true, QStringLiteral("modules"), QStringLiteral("navigate.module"), module_id, {},
               QStringLiteral("Open Module"));
  return result.ok;
}
bool LauncherBridge::unlinkModuleGame(const QString& module_id) {
  const auto result = backend_->modules().unlinkGame(module_id);
  notifyResult(result, true, QStringLiteral("modules"), QStringLiteral("navigate.module"), module_id, {},
               QStringLiteral("Open Module"));
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
  notifyResult(result, true, QStringLiteral("modules"), QStringLiteral("navigate.module"), module_id, {},
               QStringLiteral("Open Module"));
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
QString LauncherBridge::createSupportBundle() {
  const auto result = backend_->diagnostics().createSupportBundle();
  notifyResult(result);
  return result.ok ? result.data.toString() : QString{};
}
QString LauncherBridge::supportBundleDirectory() const { return backend_->diagnostics().supportBundleDirectory(); }
QString LauncherBridge::diagnosticsDirectory() const { return backend_->diagnostics().diagnosticsDirectory(); }
QString LauncherBridge::startupLogPath() const { return backend_->diagnostics().startupLogPath(); }
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
