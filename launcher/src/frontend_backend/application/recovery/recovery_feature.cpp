#include "recovery_feature.hpp"

namespace xenon::launcher::frontend_backend {

RecoveryFeature::RecoveryFeature(RecoveryService& recovery, QObject* parent)
    : QObject(parent), recovery_(recovery) {
  connect(&recovery_, &RecoveryService::changed, this, &RecoveryFeature::changed);
}

QVariantMap RecoveryFeature::state() const { return recovery_.state(); }
bool RecoveryFeature::safeMode() const { return recovery_.safeMode(); }
QString RecoveryFeature::recoveryDirectory() const { return recovery_.recoveryDirectory(); }
ServiceResult RecoveryFeature::acknowledge() { return recovery_.acknowledge(); }
ServiceResult RecoveryFeature::markInteractive() { return recovery_.markInteractive(); }
ServiceResult RecoveryFeature::restartInSafeMode() { return recovery_.restart(true); }
ServiceResult RecoveryFeature::restartNormally() {
  const auto acknowledged = recovery_.acknowledge();
  if (!acknowledged.ok) return acknowledged;
  return recovery_.restart(false);
}

}  // namespace xenon::launcher::frontend_backend
