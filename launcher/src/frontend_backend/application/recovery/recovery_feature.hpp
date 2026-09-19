#pragma once

#include "../../../services/recovery_service.hpp"

#include <QObject>
#include <QString>
#include <QVariantMap>

namespace xenon::launcher::frontend_backend {

class RecoveryFeature final : public QObject {
  Q_OBJECT

 public:
  explicit RecoveryFeature(RecoveryService& recovery, QObject* parent = nullptr);

  [[nodiscard]] QVariantMap state() const;
  [[nodiscard]] bool safeMode() const;
  [[nodiscard]] QString recoveryDirectory() const;

  [[nodiscard]] ServiceResult acknowledge();
  [[nodiscard]] ServiceResult markInteractive();
  [[nodiscard]] ServiceResult restartInSafeMode();
  [[nodiscard]] ServiceResult restartNormally();

 signals:
  void changed();

 private:
  RecoveryService& recovery_;
};

}  // namespace xenon::launcher::frontend_backend
