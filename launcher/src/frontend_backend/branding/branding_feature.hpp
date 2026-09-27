#pragma once

#include <QString>

namespace xenon::launcher::frontend_backend {

class BrandingFeature final {
 public:
  [[nodiscard]] QString themedDataUrl(const QString& asset_name, const QString& color) const;
};

}  // namespace xenon::launcher::frontend_backend
