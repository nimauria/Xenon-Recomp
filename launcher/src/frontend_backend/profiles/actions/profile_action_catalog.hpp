#pragma once

#include <QVariantList>
#include <QVariantMap>

namespace xenon::launcher::frontend_backend {

class ProfileActionCatalog final {
 public:
  [[nodiscard]] static QVariantList actions(const QVariantMap& profile, int profile_count);
  [[nodiscard]] static QVariantList backgroundActions();
};

}  // namespace xenon::launcher::frontend_backend
