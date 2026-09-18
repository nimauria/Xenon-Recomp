#pragma once

#include "../../../services/service_result.hpp"

#include <QString>

namespace xenon::launcher::frontend_backend {

class UpdateInstaller final {
 public:
  [[nodiscard]] static bool platformSupported() noexcept;
  [[nodiscard]] ServiceResult installAndRestart(const QString& package_path,
                                                const QString& version) const;
};

}  // namespace xenon::launcher::frontend_backend
