#pragma once

#include "service_result.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace xenon::launcher {

class RecoveryService final : public QObject {
  Q_OBJECT

 public:
  explicit RecoveryService(QObject* parent = nullptr);

  // Called by main() after the application identity is configured but before
  // startup logging truncates the active log. Detects an orphaned run marker,
  // preserves the previous log and creates the marker for the current process.
  static void bootstrap(const QStringList& arguments);
  static void updateBootstrapPhase(const QString& phase);
  static void markProcessCleanExit();

  [[nodiscard]] QVariantMap state() const;
  [[nodiscard]] bool safeMode() const;
  [[nodiscard]] bool recoveryPending() const;
  [[nodiscard]] QString recoveryDirectory() const;

  [[nodiscard]] ServiceResult acknowledge();
  [[nodiscard]] ServiceResult markInteractive();
  [[nodiscard]] ServiceResult restart(bool safe_mode);

 signals:
  void changed();

 private:
  [[nodiscard]] QVariantMap loadState() const;
  [[nodiscard]] bool saveState(const QVariantMap& state) const;
};

}  // namespace xenon::launcher
