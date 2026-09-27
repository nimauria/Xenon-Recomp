#pragma once

#include "../../runtime/runtime_bridge.hpp"

#include <QStringList>
#include <QVariantMap>

namespace xenon::launcher::frontend_backend {

class RuntimeFeature final {
 public:
  explicit RuntimeFeature(IRuntimeBridge& runtime);

  [[nodiscard]] bool connected() const noexcept;
  [[nodiscard]] QString status() const;
  [[nodiscard]] QVariantMap capabilities() const;
  [[nodiscard]] bool capability(const QString& id) const;
  [[nodiscard]] QStringList availableGraphicsBackends(bool test_mode) const;
  // Live status of whatever the runtime host is currently doing (module
  // version, recompilation/native-extension state, renderer, subsystems,
  // unresolved imports, last error), read from its status.json. See
  // docs/runtime/RUNTIME_HOST.md. Distinct from SessionController's play-flow state,
  // which only tracks the launcher-side prepare/validate/start phases.
  [[nodiscard]] QVariantMap gameStatus() const;
  // Tail of the runtime host's log file for the current/last session.
  [[nodiscard]] QString runtimeLog() const;

 private:
  IRuntimeBridge& runtime_;
};

}  // namespace xenon::launcher::frontend_backend
