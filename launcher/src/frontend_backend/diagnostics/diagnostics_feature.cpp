#include "diagnostics_feature.hpp"

#include "../application/application_feature.hpp"
#include "../settings/appearance/appearance_feature.hpp"
#include "../profiles/profiles_feature.hpp"
#include "../runtime/runtime_feature.hpp"
#include "../settings/settings_feature.hpp"

#include <QGuiApplication>
#include <QScreen>
#include <QSysInfo>
#include <QThread>
#include <QtGlobal>

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

namespace xenon::launcher::frontend_backend {
namespace {
QString friendlyArchitecture(QString architecture) {
  architecture = architecture.toLower();
  if (architecture == QStringLiteral("x86_64") || architecture == QStringLiteral("amd64"))
    return QStringLiteral("x86-64");
  if (architecture == QStringLiteral("arm64") || architecture == QStringLiteral("aarch64"))
    return QStringLiteral("ARM64");
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
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || factory == nullptr)
    return QStringLiteral("Unavailable");

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
    if (SUCCEEDED(fallback->GetDesc1(&desc))) result = QString::fromWCharArray(desc.Description).trimmed();
  }
  if (fallback != nullptr) fallback->Release();
  factory->Release();
  return result;
#else
  return QStringLiteral("Unavailable");
#endif
}
}  // namespace

DiagnosticsFeature::DiagnosticsFeature(ApplicationFeature& application, AppearanceFeature& appearance,
                                       RuntimeFeature& runtime, ProfilesFeature& profiles, SettingsFeature& settings)
    : application_(application), appearance_(appearance), runtime_(runtime), profiles_(profiles), settings_(settings) {}

QString DiagnosticsFeature::version() const { return QStringLiteral(XENON_LAUNCHER_VERSION); }
QString DiagnosticsFeature::hostArchitecture() const { return friendlyArchitecture(QSysInfo::currentCpuArchitecture()); }

QString DiagnosticsFeature::platformName() const {
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

QString DiagnosticsFeature::qtVersion() const { return QString::fromLatin1(qVersion()); }

QString DiagnosticsFeature::developerDiagnostics() const {
  const auto* screen = QGuiApplication::primaryScreen();
  const auto geometry = screen != nullptr ? screen->geometry() : QRect{};
  const auto refresh = screen != nullptr ? screen->refreshRate() : 0.0;
  const auto dpr = screen != nullptr ? screen->devicePixelRatio() : 1.0;
  const auto fixture = settings_.stringValue(QStringLiteral("developer/fixtureMode"), QStringLiteral("none"));
  const auto renderer = settings_.stringValue(QStringLiteral("runtime/graphicsBackend"), QStringLiteral("Automatic"));
  const auto caps = runtime_.capabilities();

  return QStringLiteral(
      "Project Xenon Launcher - Developer Diagnostics\n"
      "Launcher version: %1\nBuild mode: %2\nQt runtime: %3\nOperating system: %4\n"
      "Kernel: %5 %6\nArchitecture: %7\nLogical CPU threads: %8\nPhysical memory: %9\n"
      "Primary graphics adapter: %10\nConfigured renderer: %11\nPrimary display: %12x%13 @ %14 Hz\n"
      "Display scale factor: %15\nRuntime bridge: %16\nMemory compiled: %17\nGraphics compiled: %18\n"
      "Memory service connected: %19\nGraphics service connected: %20\nGeneric session launch: %21\n"
      "Fixture mode: %22\nTheme: %23\nAccent: %24\nSystem dark mode: %25\nSystem high contrast: %26\n"
      "Text scale: %27%\nReduce motion: %28\nHigh contrast override: %29\nSidebar mode: %30\n")
      .arg(version())
#if defined(QT_DEBUG)
      .arg(QStringLiteral("Debug"))
#else
      .arg(QStringLiteral("Release/RelWithDebInfo"))
#endif
      .arg(qtVersion()).arg(QSysInfo::prettyProductName()).arg(QSysInfo::kernelType())
      .arg(QSysInfo::kernelVersion()).arg(hostArchitecture()).arg(QThread::idealThreadCount())
      .arg(hostMemorySummary()).arg(primaryGraphicsAdapter()).arg(renderer)
      .arg(geometry.width()).arg(geometry.height()).arg(refresh, 0, 'f', 1).arg(dpr, 0, 'f', 2)
      .arg(runtime_.status())
      .arg(caps.value(QStringLiteral("memoryCompiled")).toBool() ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(caps.value(QStringLiteral("graphicsCompiled")).toBool() ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(caps.value(QStringLiteral("memory")).toBool() ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(caps.value(QStringLiteral("graphics")).toBool() ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(caps.value(QStringLiteral("sessionLaunch")).toBool() ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(fixture).arg(appearance_.themeId()).arg(appearance_.accentId())
      .arg(application_.systemDark() ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(application_.systemHighContrast() ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(qRound(settings_.numberValue(QStringLiteral("accessibility/textScale"), 1.0) * 100.0))
      .arg(settings_.boolValue(QStringLiteral("accessibility/reduceMotion"), false) ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(settings_.boolValue(QStringLiteral("accessibility/highContrast"), false) ? QStringLiteral("yes") : QStringLiteral("no"))
      .arg(settings_.stringValue(QStringLiteral("general/sidebarMode"), QStringLiteral("Auto")));
}

QString DiagnosticsFeature::userDiagnostics() const {
  return QStringLiteral(
      "Project Xenon Launcher\nVersion: %1\nSystem: %2 (%3)\nQt: %4\nRuntime status: %5\nTheme: %6\nActive profile: %7\n")
      .arg(version()).arg(QSysInfo::prettyProductName()).arg(hostArchitecture()).arg(qtVersion())
      .arg(runtime_.status()).arg(appearance_.themeId())
      .arg(profiles_.activeProfile().value(QStringLiteral("profileName"), QStringLiteral("Profile")).toString());
}

}  // namespace xenon::launcher::frontend_backend
