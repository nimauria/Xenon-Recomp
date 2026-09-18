#include "content_probe.hpp"

namespace xenon::launcher {
namespace {
ServiceResult unavailable(const QString& operation) {
  return ServiceResult::failure(
      QStringLiteral("Content probe unavailable"),
      QStringLiteral("%1 is ready at the launcher boundary, but Xbox 360 content "
                     "identification is waiting for the Xenon filesystem/content subsystem.")
          .arg(operation));
}
}  // namespace

QString UnavailableContentProbe::status() const {
  return QStringLiteral("Waiting for Xenon content subsystem");
}

ServiceResult UnavailableContentProbe::identifyGame(
    const QUrl&, const QVariantList&) const {
  return unavailable(QStringLiteral("Game import"));
}

ServiceResult UnavailableContentProbe::identifyDlc(
    const QUrl&, const QString&, const QVariantList&) const {
  return unavailable(QStringLiteral("DLC import"));
}

ServiceResult UnavailableContentProbe::materializeDlc(
    const QUrl&, const QString&, const QVariantMap&) const {
  return unavailable(QStringLiteral("DLC installation"));
}

}  // namespace xenon::launcher
