#include "runtime_bridge.hpp"

#include "xenon/core/runtime.hpp"

#include <QFileInfo>

#ifndef XENON_LAUNCHER_RUNTIME_MEMORY
#define XENON_LAUNCHER_RUNTIME_MEMORY 0
#endif
#ifndef XENON_LAUNCHER_RUNTIME_GRAPHICS
#define XENON_LAUNCHER_RUNTIME_GRAPHICS 0
#endif
#ifndef XENON_LAUNCHER_RUNTIME_AUDIO
#define XENON_LAUNCHER_RUNTIME_AUDIO 0
#endif
#ifndef XENON_LAUNCHER_RUNTIME_INPUT
#define XENON_LAUNCHER_RUNTIME_INPUT 0
#endif
#ifndef XENON_LAUNCHER_RUNTIME_NETWORK
#define XENON_LAUNCHER_RUNTIME_NETWORK 0
#endif
#ifndef XENON_LAUNCHER_RUNTIME_VULKAN
#define XENON_LAUNCHER_RUNTIME_VULKAN 0
#endif
#ifndef XENON_LAUNCHER_RUNTIME_D3D12
#define XENON_LAUNCHER_RUNTIME_D3D12 0
#endif

namespace xenon::launcher {

RuntimeBridge::RuntimeBridge() : runtime_(std::make_unique<xenon::Runtime>()) {}
RuntimeBridge::~RuntimeBridge() = default;

ServiceResult RuntimeBridge::connect() {
  if (connected()) return ServiceResult::success(QStringLiteral("Runtime connected"));

  xenon::RuntimeConfig config{};
  config.enable_logging = true;
#if XENON_LAUNCHER_RUNTIME_GRAPHICS
  config.enable_graphics = true;
#endif
#if XENON_LAUNCHER_RUNTIME_AUDIO
  config.enable_audio = true;
#endif
#if XENON_LAUNCHER_RUNTIME_INPUT
  config.enable_input = true;
#endif
#if XENON_LAUNCHER_RUNTIME_NETWORK
  config.enable_network = true;
#endif

  if (!runtime_->initialize(config)) {
    return ServiceResult::failure(QStringLiteral("Runtime connection"),
                                  QStringLiteral("The Xenon runtime core did not initialize."));
  }
  return ServiceResult::success(QStringLiteral("Runtime connected"),
                                QStringLiteral("The launcher is connected to the in-process Xenon runtime core."));
}

void RuntimeBridge::disconnect() {
  if (runtime_) runtime_->shutdown();
}

bool RuntimeBridge::connected() const noexcept {
  return runtime_ != nullptr && runtime_->is_initialized();
}

QString RuntimeBridge::status() const {
  return connected() ? QStringLiteral("Connected") : QStringLiteral("Disconnected");
}

QVariantMap RuntimeBridge::capabilities() const {
  QVariantMap result;
  result.insert(QStringLiteral("core"), true);
  result.insert(QStringLiteral("connected"), connected());
  // These report what is present in this Xenon build. They intentionally do
  // not imply that Runtime already exposes a live subsystem/service registry.
  result.insert(QStringLiteral("memoryCompiled"), static_cast<bool>(XENON_LAUNCHER_RUNTIME_MEMORY));
  result.insert(QStringLiteral("graphicsCompiled"), static_cast<bool>(XENON_LAUNCHER_RUNTIME_GRAPHICS));
  result.insert(QStringLiteral("audioCompiled"), static_cast<bool>(XENON_LAUNCHER_RUNTIME_AUDIO));
  result.insert(QStringLiteral("inputCompiled"), static_cast<bool>(XENON_LAUNCHER_RUNTIME_INPUT));
  result.insert(QStringLiteral("networkCompiled"), static_cast<bool>(XENON_LAUNCHER_RUNTIME_NETWORK));
  result.insert(QStringLiteral("vulkanCompiled"), static_cast<bool>(XENON_LAUNCHER_RUNTIME_VULKAN));
  result.insert(QStringLiteral("d3d12Compiled"), static_cast<bool>(XENON_LAUNCHER_RUNTIME_D3D12));
  result.insert(QStringLiteral("memory"), false);
  result.insert(QStringLiteral("graphics"), false);
  result.insert(QStringLiteral("audio"), false);
  result.insert(QStringLiteral("input"), false);
  result.insert(QStringLiteral("network"), false);
  result.insert(QStringLiteral("sessionLaunch"), false);
  return result;
}

ServiceResult RuntimeBridge::prepareLaunch(const LaunchConfiguration& configuration) const {
  if (!connected()) {
    return ServiceResult::failure(QStringLiteral("Runtime unavailable"),
                                  QStringLiteral("The Xenon runtime core is not connected."));
  }
  const auto& content = configuration.content_path;
  if (content.trimmed().isEmpty() || !QFileInfo::exists(content)) {
    return ServiceResult::failure(QStringLiteral("Game content missing"),
                                  QStringLiteral("The launch configuration does not point to existing local game content."));
  }
  if (configuration.module_id.trimmed().isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Game module required"),
                                  QStringLiteral("A compatible Xenon game module must identify the content before it can launch."));
  }
  return ServiceResult::success(QStringLiteral("Launch configuration ready"),
                                QStringLiteral("The launcher configuration passed the runtime-bridge validation boundary."),
                                configuration.toVariantMap());
}

ServiceResult RuntimeBridge::launch(const LaunchConfiguration& configuration) {
  const auto prepared = prepareLaunch(configuration);
  if (!prepared.ok) return prepared;

  // Runtime initialization is genuinely connected today, but the generic
  // session/executable handoff is intentionally not faked. This is the only
  // method that needs to change when Xenon exposes its game-session API.
  return ServiceResult::failure(
      QStringLiteral("Runtime session API pending"),
      QStringLiteral("The configuration reached Xenon Core successfully, but the generic game-session execution API is not implemented in Xenon Core yet."),
      configuration.toVariantMap());
}

ServiceResult RuntimeBridge::stop() {
  return ServiceResult::failure(QStringLiteral("Runtime session API pending"),
                                QStringLiteral("There is no active generic game session to stop yet."));
}

}  // namespace xenon::launcher
