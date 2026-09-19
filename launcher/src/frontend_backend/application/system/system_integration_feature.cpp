#include "system_integration_feature.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QRect>
#include <QScreen>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace xenon::launcher::frontend_backend {
namespace {

constexpr int kDefaultWidth = 1600;
constexpr int kDefaultHeight = 900;
constexpr int kMinimumWidth = 1100;
constexpr int kMinimumHeight = 700;

QScreen* bestScreenForRectangle(const QRect& rectangle) {
  QScreen* best = nullptr;
  qint64 best_area = 0;
  for (auto* screen : QGuiApplication::screens()) {
    if (screen == nullptr) continue;
    const auto visible = rectangle.intersected(screen->availableGeometry());
    const auto area = static_cast<qint64>(qMax(0, visible.width())) * qMax(0, visible.height());
    if (visible.width() >= 160 && visible.height() >= 120 && area > best_area) {
      best = screen;
      best_area = area;
    }
  }
  return best;
}

QRect fitToScreen(const QRect& rectangle, QScreen* screen) {
  if (screen == nullptr) return rectangle;
  const auto available = screen->availableGeometry();
  const auto width = qMin(rectangle.width(), available.width());
  const auto height = qMin(rectangle.height(), available.height());
  const auto max_x = available.right() - width + 1;
  const auto max_y = available.bottom() - height + 1;
  return QRect{qBound(available.left(), rectangle.x(), max_x),
               qBound(available.top(), rectangle.y(), max_y), width, height};
}

QRect centeredDefaultRect(int width, int height) {
  auto* screen = QGuiApplication::primaryScreen();
  if (screen == nullptr) return QRect{0, 0, width, height};
  const auto available = screen->availableGeometry();
  const auto bounded_width = qMin(width, available.width());
  const auto bounded_height = qMin(height, available.height());
  return QRect{available.x() + (available.width() - bounded_width) / 2,
               available.y() + (available.height() - bounded_height) / 2,
               bounded_width, bounded_height};
}

QString firstPositionalUrl(const QStringList& arguments) {
  for (const auto& argument : arguments) {
    const auto trimmed = argument.trimmed();
    if (trimmed.startsWith(QStringLiteral("xenon://"), Qt::CaseInsensitive)) return trimmed;
  }
  return {};
}

QString argumentValue(const QStringList& arguments, const QString& name) {
  for (int i = 0; i < arguments.size(); ++i) {
    const auto current = arguments.at(i);
    const auto prefix = name + QLatin1Char('=');
    if (current.startsWith(prefix)) return current.mid(prefix.size()).trimmed();
    if (current == name && i + 1 < arguments.size()) return arguments.at(i + 1).trimmed();
  }
  return {};
}

bool containsArgument(const QStringList& arguments, const QString& name) {
  return std::any_of(arguments.cbegin(), arguments.cend(), [&](const QString& value) {
    return value == name || value.startsWith(name + QLatin1Char('='));
  });
}

#ifdef Q_OS_WIN
bool writeRegistryString(const wchar_t* subkey, const wchar_t* value_name, const QString& value) {
  HKEY key = nullptr;
  const auto created = RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0, KEY_SET_VALUE,
                                       nullptr, &key, nullptr);
  if (created != ERROR_SUCCESS || key == nullptr) return false;
  const auto utf16 = reinterpret_cast<const wchar_t*>(value.utf16());
  const auto bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
  const auto written = RegSetValueExW(key, value_name, 0, REG_SZ,
                                      reinterpret_cast<const BYTE*>(utf16), bytes);
  RegCloseKey(key);
  return written == ERROR_SUCCESS;
}

QString readRegistryDefault(const wchar_t* subkey) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS ||
      key == nullptr) {
    return {};
  }

  DWORD type = 0;
  DWORD bytes = 0;
  if (RegQueryValueExW(key, nullptr, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
      type != REG_SZ || bytes < sizeof(wchar_t)) {
    RegCloseKey(key);
    return {};
  }
  QByteArray buffer(static_cast<qsizetype>(bytes), '\0');
  if (RegQueryValueExW(key, nullptr, nullptr, &type,
                       reinterpret_cast<BYTE*>(buffer.data()), &bytes) != ERROR_SUCCESS) {
    RegCloseKey(key);
    return {};
  }
  RegCloseKey(key);
  return QString::fromWCharArray(reinterpret_cast<const wchar_t*>(buffer.constData())).trimmed();
}
#endif

}  // namespace

SystemIntegrationFeature::SystemIntegrationFeature(SettingsService& settings, bool safe_mode,
                                                   QObject* parent)
    : QObject(parent), settings_(settings), safe_mode_(safe_mode) {
  connect(&settings_, &SettingsService::changed, this, [this](const QString& key, const QVariant& value) {
    if (!key.startsWith(QStringLiteral("frontend/system/"))) return;
    if (key == QStringLiteral("frontend/system/rememberWindowGeometry") && !value.toBool()) {
      (void)resetWindowState();
      return;
    }
    emit changed();
  });
}

QVariantMap SystemIntegrationFeature::normalizedWindowState() const {
  const auto remember = settings_.boolValue(QStringLiteral("frontend/system/rememberWindowGeometry"), true);
  const auto restore_maximized = settings_.boolValue(QStringLiteral("frontend/system/restoreMaximized"), true);
  const auto start_minimized = !safe_mode_ && settings_.boolValue(QStringLiteral("frontend/system/startMinimized"), false);

  if (!remember) {
    return {{QStringLiteral("hasPosition"), false},
            {QStringLiteral("x"), 0},
            {QStringLiteral("y"), 0},
            {QStringLiteral("width"), kDefaultWidth},
            {QStringLiteral("height"), kDefaultHeight},
            {QStringLiteral("maximized"), false},
            {QStringLiteral("startMinimized"), start_minimized}};
  }

  auto width = qMax(kMinimumWidth, settings_.intValue(QStringLiteral("ui/window/width"), kDefaultWidth));
  auto height = qMax(kMinimumHeight, settings_.intValue(QStringLiteral("ui/window/height"), kDefaultHeight));
  const auto has_position = settings_.value(QStringLiteral("ui/window/x")).isValid() &&
                            settings_.value(QStringLiteral("ui/window/y")).isValid();
  auto x = settings_.intValue(QStringLiteral("ui/window/x"), 0);
  auto y = settings_.intValue(QStringLiteral("ui/window/y"), 0);

  QRect rectangle{x, y, width, height};
  auto* target_screen = has_position ? bestScreenForRectangle(rectangle) : nullptr;
  if (target_screen == nullptr) {
    rectangle = centeredDefaultRect(width, height);
  } else {
    rectangle = fitToScreen(rectangle, target_screen);
  }
  x = rectangle.x();
  y = rectangle.y();
  width = rectangle.width();
  height = rectangle.height();

  return {{QStringLiteral("hasPosition"), true},
          {QStringLiteral("x"), x},
          {QStringLiteral("y"), y},
          {QStringLiteral("width"), width},
          {QStringLiteral("height"), height},
          {QStringLiteral("maximized"), restore_maximized &&
                                            settings_.boolValue(QStringLiteral("ui/window/maximized"), false)},
          {QStringLiteral("startMinimized"), start_minimized}};
}

QVariantMap SystemIntegrationFeature::windowState() const { return normalizedWindowState(); }

QVariantMap SystemIntegrationFeature::state() const {
  const auto window = normalizedWindowState();
#ifdef Q_OS_WIN
  constexpr bool url_protocol_supported = true;
#else
  constexpr bool url_protocol_supported = false;
#endif
  return {{QStringLiteral("singleInstance"), true},
          {QStringLiteral("urlProtocolSupported"), url_protocol_supported},
          {QStringLiteral("urlProtocolRegistered"), urlProtocolRegistered()},
          {QStringLiteral("protocol"), QStringLiteral("xenon://")},
          {QStringLiteral("rememberWindowGeometry"),
           settings_.boolValue(QStringLiteral("frontend/system/rememberWindowGeometry"), true)},
          {QStringLiteral("restoreMaximized"),
           settings_.boolValue(QStringLiteral("frontend/system/restoreMaximized"), true)},
          {QStringLiteral("startMinimized"), window.value(QStringLiteral("startMinimized"))}};
}

bool SystemIntegrationFeature::urlProtocolRegistered() const {
#ifdef Q_OS_WIN
  const auto command = readRegistryDefault(L"Software\\Classes\\xenon\\shell\\open\\command");
  if (command.isEmpty()) return false;
  auto executable = QFileInfo{QCoreApplication::applicationFilePath()}.canonicalFilePath();
  if (executable.isEmpty()) executable = QCoreApplication::applicationFilePath();
  return !executable.isEmpty() && command.contains(executable, Qt::CaseInsensitive);
#else
  return false;
#endif
}

ServiceResult SystemIntegrationFeature::setUrlProtocolRegistered(bool registered) {
#ifdef Q_OS_WIN
  if (!registered) {
    const auto result = RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\xenon");
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) {
      return ServiceResult::failure(QStringLiteral("Xenon links unavailable"),
                                    QStringLiteral("Windows could not remove the xenon:// protocol registration."));
    }
    emit changed();
    return ServiceResult::success(QStringLiteral("Xenon links disabled"),
                                  QStringLiteral("Windows will no longer open xenon:// links with this launcher."));
  }

  auto executable = QFileInfo{QCoreApplication::applicationFilePath()}.canonicalFilePath();
  if (executable.isEmpty()) executable = QCoreApplication::applicationFilePath();
  if (executable.trimmed().isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Xenon links unavailable"),
                                  QStringLiteral("The launcher executable path could not be resolved."));
  }

  const auto command = QStringLiteral("\"%1\" \"%2\"").arg(executable, QStringLiteral("%1"));
  const auto icon = QStringLiteral("\"%1\",0").arg(executable);
  const auto ok = writeRegistryString(L"Software\\Classes\\xenon", nullptr,
                                      QStringLiteral("URL:Xenon Launcher Protocol")) &&
                  writeRegistryString(L"Software\\Classes\\xenon", L"URL Protocol", QString{}) &&
                  writeRegistryString(L"Software\\Classes\\xenon\\DefaultIcon", nullptr, icon) &&
                  writeRegistryString(L"Software\\Classes\\xenon\\shell\\open\\command",
                                      nullptr, command);
  if (!ok || !urlProtocolRegistered()) {
    return ServiceResult::failure(QStringLiteral("Xenon links unavailable"),
                                  QStringLiteral("Windows did not accept the xenon:// protocol registration."));
  }
  emit changed();
  return ServiceResult::success(QStringLiteral("Xenon links enabled"),
                                QStringLiteral("xenon:// links will now open in Xenon Launcher."));
#else
  Q_UNUSED(registered)
  return ServiceResult::failure(QStringLiteral("Xenon links unavailable"),
                                QStringLiteral("Automatic protocol registration is currently implemented for Windows only."));
#endif
}

ServiceResult SystemIntegrationFeature::saveWindowState(int x, int y, int width, int height,
                                                        bool maximized) {
  if (safe_mode_) return ServiceResult::success();
  settings_.setValue(QStringLiteral("ui/window/maximized"), maximized);
  if (!settings_.boolValue(QStringLiteral("frontend/system/rememberWindowGeometry"), true)) {
    return ServiceResult::success();
  }

  if (!maximized && width >= kMinimumWidth && height >= kMinimumHeight) {
    settings_.setValue(QStringLiteral("ui/window/x"), x);
    settings_.setValue(QStringLiteral("ui/window/y"), y);
    settings_.setValue(QStringLiteral("ui/window/width"), width);
    settings_.setValue(QStringLiteral("ui/window/height"), height);
  }
  return ServiceResult::success();
}

ServiceResult SystemIntegrationFeature::resetWindowState() {
  for (const auto& key : {QStringLiteral("ui/window/x"), QStringLiteral("ui/window/y"),
                          QStringLiteral("ui/window/width"), QStringLiteral("ui/window/height"),
                          QStringLiteral("ui/window/maximized")}) {
    settings_.remove(key);
  }
  emit changed();
  return ServiceResult::success(QStringLiteral("Window layout reset"),
                                QStringLiteral("Xenon will use its default window size and position."));
}

ServiceResult SystemIntegrationFeature::navigateArea(const QString& area, const QString& target,
                                                     const QString& section) {
  const auto normalized = area.trimmed().toLower();
  if (normalized == QStringLiteral("home")) {
    if (safe_mode_) return ServiceResult::failure(QStringLiteral("Safe Mode"), QStringLiteral("Home is not loaded in Safe Mode."));
    emit navigationRequested(4, {}, {});
  } else if (normalized == QStringLiteral("library") || normalized == QStringLiteral("game")) {
    if (safe_mode_) return ServiceResult::failure(QStringLiteral("Safe Mode"), QStringLiteral("Library is not loaded in Safe Mode."));
    emit navigationRequested(0, target, section);
  } else if (normalized == QStringLiteral("modules") || normalized == QStringLiteral("module")) {
    if (safe_mode_) return ServiceResult::failure(QStringLiteral("Safe Mode"), QStringLiteral("Modules are not loaded in Safe Mode."));
    emit navigationRequested(1, target, section);
  } else if (normalized == QStringLiteral("profiles") || normalized == QStringLiteral("profile")) {
    if (safe_mode_) return ServiceResult::failure(QStringLiteral("Safe Mode"), QStringLiteral("Profiles are not loaded in Safe Mode."));
    emit navigationRequested(2, target, section);
  } else if (normalized == QStringLiteral("settings")) {
    emit navigationRequested(3, target, section);
  } else {
    return ServiceResult::failure(QStringLiteral("Unsupported Xenon link"),
                                  QStringLiteral("The launcher does not recognise '%1'.").arg(area));
  }
  emit activationRequested();
  return ServiceResult::success();
}

ServiceResult SystemIntegrationFeature::handleUrl(const QString& raw_url) {
  const QUrl url{raw_url};
  if (!url.isValid() || url.scheme().compare(QStringLiteral("xenon"), Qt::CaseInsensitive) != 0) {
    return ServiceResult::failure(QStringLiteral("Invalid Xenon link"),
                                  QStringLiteral("The supplied URL is not a valid xenon:// link."));
  }

  auto area = url.host().trimmed().toLower();
  auto segments = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
  QUrlQuery query{url};
  auto target = query.queryItemValue(QStringLiteral("id")).trimmed();
  auto section = query.queryItemValue(QStringLiteral("section")).trimmed();

  if (area.isEmpty() && !segments.isEmpty()) {
    area = segments.takeFirst().toLower();
  }
  if (target.isEmpty() && !segments.isEmpty()) target = QUrl::fromPercentEncoding(segments.takeFirst().toUtf8());

  if (area == QStringLiteral("modules") && target.compare(QStringLiteral("catalog"), Qt::CaseInsensitive) == 0) {
    target.clear();
    section = QStringLiteral("catalog");
  }

  return navigateArea(area, target, section);
}

ServiceResult SystemIntegrationFeature::handleArguments(const QStringList& arguments) {
  const auto url = firstPositionalUrl(arguments);
  if (!url.isEmpty()) return handleUrl(url);

  if (containsArgument(arguments, QStringLiteral("--start-minimized"))) {
    if (!safe_mode_) emit minimizeRequested();
    else emit activationRequested();
    return ServiceResult::success();
  }

  const auto game = argumentValue(arguments, QStringLiteral("--game"));
  if (!game.isEmpty()) return navigateArea(QStringLiteral("library"), game);
  const auto module = argumentValue(arguments, QStringLiteral("--module"));
  if (!module.isEmpty()) return navigateArea(QStringLiteral("modules"), module);
  const auto profile = argumentValue(arguments, QStringLiteral("--profile"));
  if (!profile.isEmpty()) return navigateArea(QStringLiteral("profiles"), profile);
  const auto settings = argumentValue(arguments, QStringLiteral("--settings"));
  if (!settings.isEmpty()) return navigateArea(QStringLiteral("settings"), settings);
  if (containsArgument(arguments, QStringLiteral("--module-catalog"))) {
    return navigateArea(QStringLiteral("modules"), {}, QStringLiteral("catalog"));
  }
  if (containsArgument(arguments, QStringLiteral("--home"))) return navigateArea(QStringLiteral("home"));
  if (containsArgument(arguments, QStringLiteral("--library"))) return navigateArea(QStringLiteral("library"));
  if (containsArgument(arguments, QStringLiteral("--modules"))) return navigateArea(QStringLiteral("modules"));
  if (containsArgument(arguments, QStringLiteral("--profiles"))) return navigateArea(QStringLiteral("profiles"));

  // A shell opening the launcher a second time with no route should simply
  // foreground the existing instance.
  emit activationRequested();
  return ServiceResult::success();
}

}  // namespace xenon::launcher::frontend_backend
