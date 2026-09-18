#include "launcher_bridge.hpp"

#include "launcher_config.hpp"

#include <QClipboard>
#include <QColor>
#include <QDesktopServices>
#include <QDir>
#include <QGuiApplication>
#include <QHash>
#include <QMetaType>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QScreen>
#include <QThread>
#include <QStyleHints>
#include <QtGlobal>
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
#include <QAccessibilityHints>
#endif
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_6.h>
#endif

#ifndef XENON_LAUNCHER_VERSION
#define XENON_LAUNCHER_VERSION "0.0.0-dev"
#endif

namespace {
constexpr auto kSettingsOrganization = "Project Xenon";
constexpr auto kSettingsApplication = "Xenon Launcher";
constexpr auto kDefaultTheme = "system";
constexpr auto kDefaultAccent = "default";
constexpr auto kDefaultCornerStyle = "rounded";
constexpr auto kDefaultProfile = "Nimauria";

QSettings settings() {
  return QSettings{kSettingsOrganization, kSettingsApplication};
}

QString friendlyArchitecture(QString architecture) {
  architecture = architecture.toLower();
  if (architecture == QStringLiteral("x86_64") ||
      architecture == QStringLiteral("amd64")) {
    return QStringLiteral("x86-64");
  }
  if (architecture == QStringLiteral("arm64") ||
      architecture == QStringLiteral("aarch64")) {
    return QStringLiteral("ARM64");
  }
  return architecture;
}

QString hostMemorySummary() {
#if defined(Q_OS_WIN)
  MEMORYSTATUSEX state{};
  state.dwLength = sizeof(state);
  if (GlobalMemoryStatusEx(&state) != FALSE) {
    constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
    return QStringLiteral("%1 GiB").arg(state.ullTotalPhys / kGiB, 0, 'f', 1);
  }
#endif
  return QStringLiteral("Unavailable");
}

QString primaryGraphicsAdapter() {
#if defined(Q_OS_WIN)
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                reinterpret_cast<void**>(&factory))) ||
      factory == nullptr) {
    return QStringLiteral("Unavailable");
  }

  QString result = QStringLiteral("Unavailable");
  IDXGIAdapter1* fallback = nullptr;
  for (UINT index = 0;; ++index) {
    IDXGIAdapter1* adapter = nullptr;
    if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
    if (adapter == nullptr) continue;

    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc))) {
      if (fallback == nullptr) {
        fallback = adapter;
        fallback->AddRef();
      }
      if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
        result = QString::fromWCharArray(desc.Description).trimmed();
        adapter->Release();
        break;
      }
    }
    adapter->Release();
  }

  if (result == QStringLiteral("Unavailable") && fallback != nullptr) {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(fallback->GetDesc1(&desc)))
      result = QString::fromWCharArray(desc.Description).trimmed();
  }
  if (fallback != nullptr) fallback->Release();
  factory->Release();
  return result;
#else
  return QStringLiteral("Unavailable");
#endif
}
}  // namespace

LauncherBridge::LauncherBridge(QObject* parent)
    : QObject(parent) {
  auto s = settings();
  theme_id_ = s.value("ui/theme", kDefaultTheme).toString();
  // Migrate IDs used by the earliest launcher mockup implementation.
  if (theme_id_ == QStringLiteral("xenon-cyan")) theme_id_ = QStringLiteral("xenon-dark");
  if (theme_id_ == QStringLiteral("xenon-green") || theme_id_ == QStringLiteral("graphite")) theme_id_ = QStringLiteral("carbon");
  if (theme_id_ == QStringLiteral("industrial-amber")) theme_id_ = QStringLiteral("industrial");
  accent_id_ = s.value("ui/accent", kDefaultAccent).toString();
  corner_style_ = s.value("ui/corners", kDefaultCornerStyle).toString();
  profile_name_ = s.value("profile/name", kDefaultProfile).toString();


  if (theme_id_.isEmpty()) theme_id_ = kDefaultTheme;
  if (accent_id_.isEmpty()) accent_id_ = kDefaultAccent;
  if (corner_style_.isEmpty()) corner_style_ = kDefaultCornerStyle;
  if (profile_name_.isEmpty()) profile_name_ = kDefaultProfile;

  if (auto* hints = QGuiApplication::styleHints()) {
    connect(hints, &QStyleHints::colorSchemeChanged, this,
            [this](Qt::ColorScheme) { emit systemAppearanceChanged(); });
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    if (const auto* accessibility = hints->accessibility()) {
      connect(accessibility, &QAccessibilityHints::contrastPreferenceChanged,
              this, [this](Qt::ContrastPreference) {
                emit systemAppearanceChanged();
              });
    }
#endif
  }
}

QString LauncherBridge::version() const {
  return QStringLiteral(XENON_LAUNCHER_VERSION);
}

bool LauncherBridge::backendConnected() const noexcept {
  // Front-end milestone. This becomes dynamic once the runtime service
  // registry is connected to the launcher.
  return false;
}

bool LauncherBridge::testMode() const noexcept {
  return xenon::launcher::kTestMode;
}

QString LauncherBridge::themeId() const { return theme_id_; }

void LauncherBridge::setThemeId(const QString& theme_id) {
  if (theme_id.isEmpty() || theme_id == theme_id_) return;
  theme_id_ = theme_id;
  settings().setValue("ui/theme", theme_id_);
  emit themeIdChanged();
}

QString LauncherBridge::accentId() const { return accent_id_; }

void LauncherBridge::setAccentId(const QString& accent_id) {
  if (accent_id.isEmpty() || accent_id == accent_id_) return;
  accent_id_ = accent_id;
  settings().setValue("ui/accent", accent_id_);
  emit accentIdChanged();
}

QString LauncherBridge::cornerStyle() const { return corner_style_; }

void LauncherBridge::setCornerStyle(const QString& corner_style) {
  if (corner_style.isEmpty() || corner_style == corner_style_) return;
  corner_style_ = corner_style;
  settings().setValue("ui/corners", corner_style_);
  emit cornerStyleChanged();
}

QString LauncherBridge::profileName() const { return profile_name_; }

void LauncherBridge::setProfileName(const QString& profile_name) {
  const auto trimmed = profile_name.trimmed();
  if (trimmed.isEmpty() || trimmed == profile_name_) return;
  profile_name_ = trimmed;
  settings().setValue("profile/name", profile_name_);
  emit profileNameChanged();
}


bool LauncherBridge::systemDark() const noexcept {
  const auto* hints = QGuiApplication::styleHints();
  return hints != nullptr && hints->colorScheme() == Qt::ColorScheme::Dark;
}

bool LauncherBridge::systemHighContrast() const noexcept {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
  const auto* hints = QGuiApplication::styleHints();
  const auto* accessibility = hints != nullptr ? hints->accessibility() : nullptr;
  return accessibility != nullptr &&
         accessibility->contrastPreference() == Qt::ContrastPreference::HighContrast;
#else
  return false;
#endif
}

QString LauncherBridge::hostArchitecture() const {
  return friendlyArchitecture(QSysInfo::currentCpuArchitecture());
}

QString LauncherBridge::platformName() const {
#if defined(Q_OS_WIN)
  return QStringLiteral("Windows");
#elif defined(Q_OS_LINUX)
  return QStringLiteral("Linux");
#elif defined(Q_OS_ANDROID)
  return QStringLiteral("Android");
#elif defined(Q_OS_MACOS)
  return QStringLiteral("macOS");
#else
  return QSysInfo::prettyProductName();
#endif
}

QString LauncherBridge::qtVersion() const {
  return QString::fromLatin1(qVersion());
}

QStringList LauncherBridge::availableGraphicsBackends() const {
  // This list is host-aware. Later the runtime capability registry will make
  // it adapter-aware too (driver/API availability, device features, etc.).
  QStringList backends{QStringLiteral("Automatic"), QStringLiteral("Vulkan")};
#if defined(Q_OS_WIN)
  backends.append(QStringLiteral("Direct3D 12"));
#endif
  return backends;
}

QString LauncherBridge::appDataPath() const {
  return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QString LauncherBridge::configPath() const {
  return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
}

QString LauncherBridge::defaultGameLibraryPath() const {
  const auto documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  return QDir{documents}.filePath(QStringLiteral("Xenon/Games"));
}

QString LauncherBridge::defaultSaveDataPath() const {
  const auto documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
  return QDir{documents}.filePath(QStringLiteral("Xenon/Saves"));
}

QString LauncherBridge::defaultProfilesPath() const {
  return QDir{appDataPath()}.filePath(QStringLiteral("Profiles"));
}

QString LauncherBridge::defaultModulesPath() const {
  return QDir{appDataPath()}.filePath(QStringLiteral("Modules"));
}

QString LauncherBridge::defaultScreenshotsPath() const {
  const auto pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
  return QDir{pictures}.filePath(QStringLiteral("Xenon"));
}

QString LauncherBridge::cachePath() const {
  return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

void LauncherBridge::notifyUnavailable(const QString& feature) {
  emit notificationRequested(
      feature,
      QStringLiteral(
          "This front-end action is ready, but the Xenon backend service for "
          "it is not connected yet."));
}

void LauncherBridge::notify(const QString& title, const QString& message) {
  emit notificationRequested(title, message);
}

void LauncherBridge::requestLauncherUpdateCheck() {
  notifyUnavailable(QStringLiteral("Launcher Update Check"));
}

void LauncherBridge::requestModuleCatalogRefresh() {
  notifyUnavailable(QStringLiteral("Refresh Module Catalog"));
}

void LauncherBridge::requestModuleUpdate(const QString& module_id) {
  const auto label = module_id.trimmed().isEmpty()
      ? QStringLiteral("Module Update")
      : QStringLiteral("Update Module %1").arg(module_id.trimmed());
  notifyUnavailable(label);
}

void LauncherBridge::requestModuleInstall(const QString& module_id) {
  const auto label = module_id.trimmed().isEmpty()
      ? QStringLiteral("Install Module")
      : QStringLiteral("Install Module %1").arg(module_id.trimmed());
  notifyUnavailable(label);
}

void LauncherBridge::rememberPage(int page_index) {
  settings().setValue("ui/page", page_index);
}

int LauncherBridge::rememberedPage() const {
  return settings().value("ui/page", 0).toInt();
}

QVariant LauncherBridge::settingValue(const QString& key,
                                      const QVariant& fallback) const {
  return settings().value(QStringLiteral("frontend/") + key, fallback);
}

bool LauncherBridge::boolSetting(const QString& key, bool fallback) const {
  const auto value = settingValue(key, fallback);
  const auto type = value.metaType().id();

  if (type == QMetaType::Bool) return value.toBool();
  if (type == QMetaType::Int || type == QMetaType::UInt ||
      type == QMetaType::LongLong || type == QMetaType::ULongLong ||
      type == QMetaType::Double || type == QMetaType::Float) {
    return value.toDouble() != 0.0;
  }

  const auto text = value.toString().trimmed().toLower();
  if (text == QStringLiteral("true") || text == QStringLiteral("1") ||
      text == QStringLiteral("yes") || text == QStringLiteral("on")) {
    return true;
  }
  if (text == QStringLiteral("false") || text == QStringLiteral("0") ||
      text == QStringLiteral("no") || text == QStringLiteral("off") ||
      text.isEmpty()) {
    return false;
  }
  return fallback;
}

QString LauncherBridge::stringSetting(const QString& key,
                                      const QString& fallback) const {
  const auto value = settingValue(key, fallback);
  return value.isValid() && !value.isNull() ? value.toString() : fallback;
}

double LauncherBridge::numberSetting(const QString& key, double fallback) const {
  bool ok = false;
  const auto number = settingValue(key, fallback).toDouble(&ok);
  return ok ? number : fallback;
}

int LauncherBridge::intSetting(const QString& key, int fallback) const {
  bool ok = false;
  const auto number = settingValue(key, fallback).toInt(&ok);
  return ok ? number : fallback;
}

void LauncherBridge::setSettingValue(const QString& key,
                                     const QVariant& value) {
  settings().setValue(QStringLiteral("frontend/") + key, value);
  emit settingChanged(key, value);
}

void LauncherBridge::resetSetting(const QString& key) {
  settings().remove(QStringLiteral("frontend/") + key);
  emit settingChanged(key, QVariant{});
}

bool LauncherBridge::featureEnabled(const QString& feature) const noexcept {
  const auto& f = xenon::launcher::kUiFeatures;

  if (feature == QStringLiteral("settings.general")) return f.settings_general;
  if (feature == QStringLiteral("settings.appearance")) return f.settings_appearance;
  if (feature == QStringLiteral("settings.library")) return f.settings_library;
  if (feature == QStringLiteral("settings.paths")) return f.settings_paths;
  if (feature == QStringLiteral("settings.runtime")) return f.settings_runtime;
  if (feature == QStringLiteral("settings.graphics")) return f.settings_graphics;
  if (feature == QStringLiteral("settings.input")) return f.settings_input;
  if (feature == QStringLiteral("settings.audio")) return f.settings_audio;
  if (feature == QStringLiteral("settings.network")) return f.settings_network;
  if (feature == QStringLiteral("settings.updates")) return f.settings_updates;
  if (feature == QStringLiteral("settings.accessibility")) return f.settings_accessibility;
  if (feature == QStringLiteral("settings.developer")) return f.settings_developer;
  if (feature == QStringLiteral("settings.about")) return f.settings_about;

  if (feature == QStringLiteral("library.dlc")) return f.library_dlc;
  if (feature == QStringLiteral("library.manageFiles")) return f.library_manage_files;
  if (feature == QStringLiteral("modules.updates")) return f.modules_updates;
  if (feature == QStringLiteral("modules.catalog")) return f.modules_catalog;
  if (feature == QStringLiteral("modules.settings")) return f.modules_settings;
  if (feature == QStringLiteral("profiles.paths")) return f.profiles_paths;
  if (feature == QStringLiteral("profiles.avatars")) return f.profiles_avatars;

  return false;
}

QString LauncherBridge::loadProfileState() const {
  const auto profile_dir = settingValue(
      QStringLiteral("paths/profiles"), defaultProfilesPath()).toString();
  const auto profile_file = QDir{profile_dir}.filePath(QStringLiteral("profiles.json"));

  QFile file{profile_file};
  if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    const auto data = QString::fromUtf8(file.readAll()).trimmed();
    if (!data.isEmpty()) return data;
  }

  // QSettings remains a migration/fallback copy so changing the profile
  // storage directory never silently loses the user's launcher profile list.
  return settings().value(QStringLiteral("frontend/profiles/state"), QString{}).toString();
}

bool LauncherBridge::saveProfileState(const QString& json) {
  settings().setValue(QStringLiteral("frontend/profiles/state"), json);

  const auto profile_dir = settingValue(
      QStringLiteral("paths/profiles"), defaultProfilesPath()).toString();
  if (profile_dir.trimmed().isEmpty()) return false;
  if (!QDir{}.mkpath(profile_dir)) {
    notify(QStringLiteral("Profile storage error"),
           QStringLiteral("Xenon could not create the configured profile folder."));
    return false;
  }

  const auto profile_file = QDir{profile_dir}.filePath(QStringLiteral("profiles.json"));
  QSaveFile file{profile_file};
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    notify(QStringLiteral("Profile storage error"),
           QStringLiteral("Xenon could not write profiles.json in the configured profile folder."));
    return false;
  }
  file.write(json.toUtf8());
  if (!file.commit()) {
    notify(QStringLiteral("Profile storage error"),
           QStringLiteral("Xenon could not commit profiles.json safely."));
    return false;
  }
  return true;
}

QString LauncherBridge::toLocalPath(const QUrl& url) const {
  if (url.isLocalFile()) return url.toLocalFile();
  return url.toString();
}

bool LauncherBridge::openFolder(const QString& path) {
  const auto clean_path = QDir::cleanPath(path.trimmed());
  if (clean_path.isEmpty()) {
    notify(QStringLiteral("Unable to open folder"),
           QStringLiteral("No folder path has been configured."));
    return false;
  }

  QDir dir{clean_path};
  if (!dir.exists() && !QDir{}.mkpath(clean_path)) {
    notify(QStringLiteral("Unable to open folder"),
           QStringLiteral("Xenon could not create the requested folder:\n%1").arg(clean_path));
    return false;
  }

  if (!QDesktopServices::openUrl(QUrl::fromLocalFile(clean_path))) {
    notify(QStringLiteral("Unable to open folder"),
           QStringLiteral("The operating system could not open:\n%1").arg(clean_path));
    return false;
  }
  return true;
}

bool LauncherBridge::openExternalUrl(const QString& url) const {
  return QDesktopServices::openUrl(QUrl{url});
}

QString LauncherBridge::developerDiagnostics() const {
  const auto* screen = QGuiApplication::primaryScreen();
  const auto geometry = screen != nullptr ? screen->geometry() : QRect{};
  const auto refresh = screen != nullptr ? screen->refreshRate() : 0.0;
  const auto dpr = screen != nullptr ? screen->devicePixelRatio() : 1.0;
  const auto fixture = stringSetting(QStringLiteral("developer/fixtureMode"),
                                     testMode() ? QStringLiteral("generic")
                                                : QStringLiteral("none"));
  const auto configured_renderer = stringSetting(QStringLiteral("runtime/renderer"),
                                                  QStringLiteral("Automatic"));

  return QStringLiteral(
      "Project Xenon Launcher - Developer Diagnostics\n"
      "Launcher version: %1\n"
      "Build mode: %2\n"
      "Qt runtime: %3\n"
      "Operating system: %4\n"
      "Kernel: %5 %6\n"
      "Architecture: %7\n"
      "Logical CPU threads: %8\n"
      "Physical memory: %9\n"
      "Primary graphics adapter: %10\n"
      "Configured renderer: %11\n"
      "Primary display: %12x%13 @ %14 Hz\n"
      "Display scale factor: %15\n"
      "Backend connected: %16\n"
      "Compiled test mode: %17\n"
      "Fixture mode: %18\n"
      "Theme: %19\n"
      "Accent: %20\n"
      "System dark mode: %21\n"
      "System high contrast: %22\n"
      "Text scale: %23%\n"
      "Reduce motion: %24\n"
      "High contrast override: %25\n"
      "Sidebar mode: %26\n")
      .arg(version())
#if defined(QT_DEBUG)
      .arg(QStringLiteral("Debug"))
#else
      .arg(QStringLiteral("Release/RelWithDebInfo"))
#endif
      .arg(qtVersion())
      .arg(QSysInfo::prettyProductName())
      .arg(QSysInfo::kernelType())
      .arg(QSysInfo::kernelVersion())
      .arg(hostArchitecture())
      .arg(QThread::idealThreadCount())
      .arg(hostMemorySummary())
      .arg(primaryGraphicsAdapter())
      .arg(configured_renderer)
      .arg(geometry.width())
      .arg(geometry.height())
      .arg(refresh, 0, 'f', 1)
      .arg(dpr, 0, 'f', 2)
      .arg(backendConnected() ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(testMode() ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(fixture)
      .arg(themeId())
      .arg(accentId())
      .arg(systemDark() ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(systemHighContrast() ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(qRound(numberSetting(QStringLiteral("accessibility/textScale"), 1.0) * 100.0))
      .arg(boolSetting(QStringLiteral("accessibility/reduceMotion"), false) ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(boolSetting(QStringLiteral("accessibility/highContrast"), false) ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(stringSetting(QStringLiteral("general/sidebarMode"), QStringLiteral("Auto")));
}

QString LauncherBridge::userDiagnostics() const {
  return QStringLiteral(
      "Project Xenon Launcher\n"
      "Version: %1\n"
      "System: %2 (%3)\n"
      "Qt: %4\n"
      "Runtime status: %5\n"
      "Theme: %6\n"
      "Active profile: %7\n")
      .arg(version())
      .arg(QSysInfo::prettyProductName())
      .arg(hostArchitecture())
      .arg(qtVersion())
      .arg(backendConnected() ? QStringLiteral("Connected") : QStringLiteral("Front-end only"))
      .arg(themeId())
      .arg(profileName());
}

void LauncherBridge::copyText(const QString& text) const {
  QGuiApplication::clipboard()->setText(text);
}

void LauncherBridge::copyDiagnostics() const {
  copyText(developerDiagnostics());
}

QString LauncherBridge::themedBrandingDataUrl(const QString& asset_name,
                                               const QString& color) const {
  static const QHash<QString, QString> assets{
      {QStringLiteral("mark"), QStringLiteral(":/branding/xenon-mark.svg")},
      {QStringLiteral("icon"), QStringLiteral(":/branding/xenon-icon.svg")},
      {QStringLiteral("wordmark"), QStringLiteral(":/branding/xenon-wordmark.svg")},
      {QStringLiteral("lockup"), QStringLiteral(":/branding/xenon-lockup.svg")},
      {QStringLiteral("lockup-full"), QStringLiteral(":/branding/xenon-lockup-full.svg")},
      {QStringLiteral("profile-mark"), QStringLiteral(":/branding/xenon-profile-mark.svg")},
  };

  const auto path = assets.value(asset_name, assets.value(QStringLiteral("mark")));
  QFile file{path};
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};

  auto svg = file.readAll();
  const auto normalized = QColor{color}.isValid() ? QColor{color}.name(QColor::HexRgb)
                                                   : QStringLiteral("#35D7EA");
  svg.replace("currentColor", normalized.toUtf8());
  return QStringLiteral("data:image/svg+xml;base64,%1")
      .arg(QString::fromLatin1(svg.toBase64()));
}

QString LauncherBridge::importProfileAvatar(const QString& profile_id,
                                             const QUrl& source_url) {
  const auto source = source_url.isLocalFile() ? source_url.toLocalFile()
                                                : source_url.toString();
  const QFileInfo source_info{source};
  if (!source_info.exists() || !source_info.isFile()) {
    notify(QStringLiteral("Profile image"),
           QStringLiteral("The selected image could not be opened."));
    return {};
  }

  auto extension = source_info.suffix().toLower();
  if (extension != QStringLiteral("png") && extension != QStringLiteral("jpg") &&
      extension != QStringLiteral("jpeg") && extension != QStringLiteral("webp")) {
    notify(QStringLiteral("Profile image"),
           QStringLiteral("Choose a PNG, JPEG, or WebP image."));
    return {};
  }
  if (extension == QStringLiteral("jpeg")) extension = QStringLiteral("jpg");

  const auto root_dir = stringSetting(QStringLiteral("paths/profiles"), defaultProfilesPath());
  const auto safe_id = profile_id.trimmed().isEmpty() ? QStringLiteral("profile-temp")
                                                       : profile_id.trimmed();
  const auto profile_dir = QDir{root_dir}.filePath(safe_id);
  if (!QDir{}.mkpath(profile_dir)) {
    notify(QStringLiteral("Profile image"),
           QStringLiteral("Xenon could not create the profile image folder."));
    return {};
  }

  removeProfileAvatar(safe_id);
  const auto destination = QDir{profile_dir}.filePath(QStringLiteral("avatar.%1").arg(extension));
  if (!QFile::copy(source, destination)) {
    notify(QStringLiteral("Profile image"),
           QStringLiteral("Xenon could not copy the selected profile image."));
    return {};
  }
  return QUrl::fromLocalFile(destination).toString();
}

bool LauncherBridge::removeProfileAvatar(const QString& profile_id) {
  const auto root_dir = stringSetting(QStringLiteral("paths/profiles"), defaultProfilesPath());
  QDir dir{QDir{root_dir}.filePath(profile_id)};
  if (!dir.exists()) return true;

  bool ok = true;
  const auto avatars = dir.entryList({QStringLiteral("avatar.*")}, QDir::Files);
  for (const auto& avatar : avatars) {
    if (!dir.remove(avatar)) ok = false;
  }
  return ok;
}
