#pragma once

#include <QVariantList>
#include <QVariantMap>

namespace xenon::launcher::frontend_backend {

class ModuleActionCatalog final {
 public:
  [[nodiscard]] static QVariantList actions(const QVariantMap& module,
                                            const QVariantMap& update_state,
                                            bool has_settings);
  [[nodiscard]] static QVariantList pageActions();
};

}  // namespace xenon::launcher::frontend_backend
