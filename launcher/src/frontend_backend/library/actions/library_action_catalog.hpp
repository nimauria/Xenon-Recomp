#pragma once

#include <QVariantList>
#include <QVariantMap>

namespace xenon::launcher::frontend_backend {

class LibraryActionCatalog final {
 public:
  [[nodiscard]] static QVariantList gameActions(const QVariantMap& game, bool test_mode);
  [[nodiscard]] static QVariantList manageActions(const QVariantMap& game, bool test_mode);
  [[nodiscard]] static QVariantList backgroundActions();
};

}  // namespace xenon::launcher::frontend_backend
