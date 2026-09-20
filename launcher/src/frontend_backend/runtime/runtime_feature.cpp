#include "runtime_feature.hpp"

namespace xenon::launcher::frontend_backend {

RuntimeFeature::RuntimeFeature(IRuntimeBridge& runtime) : runtime_(runtime) {}

bool RuntimeFeature::connected() const noexcept { return runtime_.connected(); }
QString RuntimeFeature::status() const { return runtime_.status(); }
QVariantMap RuntimeFeature::capabilities() const { return runtime_.capabilities(); }

bool RuntimeFeature::capability(const QString& id) const {
  return runtime_.capabilities().value(id).toBool();
}

QVariantMap RuntimeFeature::gameStatus() const { return runtime_.sessionStatus(); }
QString RuntimeFeature::runtimeLog() const { return runtime_.runtimeLog(); }

QStringList RuntimeFeature::availableGraphicsBackends(bool test_mode) const {
  QStringList backends{QStringLiteral("Automatic")};
  const auto caps = runtime_.capabilities();
  if (test_mode || caps.value(QStringLiteral("vulkanCompiled")).toBool()) {
    backends.append(QStringLiteral("Vulkan"));
  }
#if defined(Q_OS_WIN)
  if (test_mode || caps.value(QStringLiteral("d3d12Compiled")).toBool()) {
    backends.append(QStringLiteral("Direct3D 12"));
  }
#endif
  return backends;
}

}  // namespace xenon::launcher::frontend_backend
