#pragma once

#include <QVariantList>
#include <QVariantMap>

namespace xenon::launcher::frontend_backend {

class DlcActionCatalog final {
 public:
  [[nodiscard]] static QVariantList actions(const QVariantMap& dlc, bool test_mode);
  [[nodiscard]] static QVariantList backgroundActions(bool has_catalogue);
};

}  // namespace xenon::launcher::frontend_backend
