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

 private:
  IRuntimeBridge& runtime_;
};

}  // namespace xenon::launcher::frontend_backend
