#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QMutex>
#include <QMutexLocker>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

#include <atomic>
#include <memory>

#include "launcher_bridge.hpp"
#include "services/recovery_service.hpp"
#include "services/single_instance_service.hpp"

#ifdef Q_OS_WIN
#include <shobjidl.h>
#endif

namespace {
std::unique_ptr<QFile> g_startupLog;
QMutex g_startupLogMutex;
std::atomic_bool g_verboseLogging{false};

QString startupLogPath() {
  auto root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (root.trimmed().isEmpty()) root = QDir::tempPath();
  QDir{}.mkpath(root);
  return QDir{root}.filePath(QStringLiteral("xenon-launcher-startup.log"));
}

void writeStartupLog(const QString& message) {
  QMutexLocker lock{&g_startupLogMutex};
  if (!g_startupLog || !g_startupLog->isOpen()) return;
  QTextStream stream{g_startupLog.get()};
  stream << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << "  " << message << '\n';
  stream.flush();
  g_startupLog->flush();
}

void qtMessageToStartupLog(QtMsgType type, const QMessageLogContext&, const QString& message) {
  if (!g_verboseLogging.load(std::memory_order_relaxed) &&
      (type == QtDebugMsg || type == QtInfoMsg)) {
    return;
  }
  QString level;
  switch (type) {
    case QtDebugMsg: level = QStringLiteral("DEBUG"); break;
    case QtInfoMsg: level = QStringLiteral("INFO"); break;
    case QtWarningMsg: level = QStringLiteral("WARNING"); break;
    case QtCriticalMsg: level = QStringLiteral("CRITICAL"); break;
    case QtFatalMsg: level = QStringLiteral("FATAL"); break;
  }
  writeStartupLog(QStringLiteral("[%1] %2").arg(level, message));
}

void beginStartupLogging() {
  g_startupLog = std::make_unique<QFile>(startupLogPath());
  if (g_startupLog->open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
    writeStartupLog(QStringLiteral("Xenon Launcher bootstrap started"));
    qInstallMessageHandler(qtMessageToStartupLog);
  } else {
    g_startupLog.reset();
  }
}
}  // namespace

int main(int argc, char* argv[]) {
  QQuickStyle::setStyle("Basic");

  QGuiApplication app(argc, argv);
  QCoreApplication::setApplicationName("Xenon Launcher");
  QCoreApplication::setOrganizationName("Project Xenon");
  QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/branding/xenon-app-icon-256.png")));
#ifdef Q_OS_WIN
  (void)SetCurrentProcessExplicitAppUserModelID(L"ProjectXenon.XenonLauncher");
#endif

  const auto startup_arguments = QCoreApplication::arguments().mid(1);
  xenon::launcher::SingleInstanceService single_instance;
  const auto instance_result = single_instance.startOrForward(startup_arguments);
  if (instance_result == xenon::launcher::SingleInstanceService::StartResult::Forwarded) {
    return EXIT_SUCCESS;
  }
  if (instance_result == xenon::launcher::SingleInstanceService::StartResult::Unavailable) {
    qWarning() << "Xenon single-instance coordination is unavailable; continuing without it";
  }

  xenon::launcher::RecoveryService::bootstrap(QCoreApplication::arguments());
  beginStartupLogging();
  xenon::launcher::RecoveryService::updateBootstrapPhase(QStringLiteral("logging-ready"));
  writeStartupLog(QStringLiteral("QGuiApplication ready"));

  xenon::launcher::RecoveryService::updateBootstrapPhase(QStringLiteral("backend-constructing"));
  LauncherBridge launcher_bridge;
  QObject::connect(&single_instance, &xenon::launcher::SingleInstanceService::argumentsReceived,
                   &launcher_bridge, &LauncherBridge::handleExternalArguments);
  xenon::launcher::RecoveryService::updateBootstrapPhase(QStringLiteral("backend-ready"));
  g_verboseLogging.store(launcher_bridge.boolSetting(QStringLiteral("developer/verboseLogging"), false),
                         std::memory_order_relaxed);
  QObject::connect(&launcher_bridge, &LauncherBridge::settingChanged, &app,
                   [](const QString& key, const QVariant& value) {
                     if (key == QStringLiteral("developer/verboseLogging")) {
                       g_verboseLogging.store(value.toBool(), std::memory_order_relaxed);
                       writeStartupLog(QStringLiteral("Verbose launcher logging %1")
                                           .arg(value.toBool() ? QStringLiteral("enabled")
                                                               : QStringLiteral("disabled")));
                     }
                   });
  writeStartupLog(QStringLiteral("LauncherBridge constructed"));
  const auto recovery_state = launcher_bridge.recoveryState();
  if (launcher_bridge.safeMode()) {
    QString safe_mode_suffix;
    if (recovery_state.value(QStringLiteral("automaticSafeMode")).toBool()) {
      const auto reason = recovery_state.value(QStringLiteral("automaticSafeModeReason")).toString();
      safe_mode_suffix = reason == QStringLiteral("startup-failure")
                             ? QStringLiteral(" (automatic after startup failure)")
                             : QStringLiteral(" (automatic after repeated unclean starts)");
    }
    writeStartupLog(QStringLiteral("Safe Mode active%1").arg(safe_mode_suffix));
  }
  if (recovery_state.value(QStringLiteral("previousUncleanShutdown")).toBool()) {
    writeStartupLog(QStringLiteral("Previous unclean shutdown detected; last phase: %1")
                        .arg(recovery_state.value(QStringLiteral("previousPhase"),
                                                  QStringLiteral("unknown")).toString()));
  }

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty("launcherBridge", &launcher_bridge);

  QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app,
                   [](const QList<QQmlError>& warnings) {
                     for (const auto& warning : warnings) {
                       writeStartupLog(QStringLiteral("[QML] %1").arg(warning.toString()));
                     }
                   });

  QObject::connect(
      &engine,
      &QQmlApplicationEngine::objectCreationFailed,
      &app,
      []() {
        writeStartupLog(QStringLiteral("Main QML object creation failed"));
        QCoreApplication::exit(EXIT_FAILURE);
      },
      Qt::QueuedConnection);

  writeStartupLog(QStringLiteral("Loading Xenon.Launcher/Main"));
  xenon::launcher::RecoveryService::updateBootstrapPhase(QStringLiteral("qml-loading"));
  engine.loadFromModule("Xenon.Launcher", "Main");
  if (engine.rootObjects().isEmpty()) {
    writeStartupLog(QStringLiteral("No root QML object was created"));
    return EXIT_FAILURE;
  }

  xenon::launcher::RecoveryService::updateBootstrapPhase(QStringLiteral("qml-root-ready"));
  writeStartupLog(QStringLiteral("Main window created; entering event loop"));
  if (!startup_arguments.isEmpty()) {
    QTimer::singleShot(0, &launcher_bridge, [&launcher_bridge, startup_arguments]() {
      launcher_bridge.handleExternalArguments(startup_arguments);
    });
  }
  const auto result = app.exec();
  writeStartupLog(QStringLiteral("Event loop exited with code %1").arg(result));
  xenon::launcher::RecoveryService::markProcessCleanExit();
  return result;
}
