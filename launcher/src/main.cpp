#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QIcon>

#include "launcher_bridge.hpp"

int main(int argc, char* argv[]) {
  QQuickStyle::setStyle("Basic");

  QGuiApplication app(argc, argv);
  QCoreApplication::setApplicationName("Xenon Launcher");
  QCoreApplication::setOrganizationName("Project Xenon");
  QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/branding/xenon-app-icon-256.png")));

  LauncherBridge launcher_bridge;

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty("launcherBridge", &launcher_bridge);

  QObject::connect(
      &engine,
      &QQmlApplicationEngine::objectCreationFailed,
      &app,
      []() { QCoreApplication::exit(EXIT_FAILURE); },
      Qt::QueuedConnection);

  engine.loadFromModule("Xenon.Launcher", "Main");
  return app.exec();
}
