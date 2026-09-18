#pragma once

#include "../services/service_result.hpp"

#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

namespace xenon::launcher {

// Framework-facing content identification seam.
//
// Launcher Core deliberately does not understand XEX/STFS/Xbox content. A
// framework implementation of this interface identifies content and, for DLC,
// materialises it into a launcher-prepared managed destination. The rest of the
// library lifecycle remains owned by Launcher Core.
class IContentProbe {
 public:
  virtual ~IContentProbe() = default;

  [[nodiscard]] virtual bool available() const noexcept = 0;
  [[nodiscard]] virtual QString status() const = 0;

  // data must be a QVariantMap containing at least "moduleId" when successful.
  [[nodiscard]] virtual ServiceResult identifyGame(
      const QUrl& source, const QVariantList& module_candidates) const = 0;

  // data must be a QVariantMap containing at least "dlcId" when successful.
  [[nodiscard]] virtual ServiceResult identifyDlc(
      const QUrl& source, const QString& game_id, const QVariantList& catalogue) const = 0;

  // Copy/extract the already-identified source into destination. The launcher
  // creates and validates destination before this call. Implementations must
  // not choose a different destination.
  [[nodiscard]] virtual ServiceResult materializeDlc(
      const QUrl& source, const QString& destination,
      const QVariantMap& identification) const = 0;
};

// Current default until the Xenon filesystem/content subsystem exposes its
// probing API. Keeping this as a real implementation means the UI and launcher
// services already exercise the final dependency direction.
class UnavailableContentProbe final : public IContentProbe {
 public:
  [[nodiscard]] bool available() const noexcept override { return false; }
  [[nodiscard]] QString status() const override;
  [[nodiscard]] ServiceResult identifyGame(
      const QUrl& source, const QVariantList& module_candidates) const override;
  [[nodiscard]] ServiceResult identifyDlc(
      const QUrl& source, const QString& game_id,
      const QVariantList& catalogue) const override;
  [[nodiscard]] ServiceResult materializeDlc(
      const QUrl& source, const QString& destination,
      const QVariantMap& identification) const override;
};

}  // namespace xenon::launcher
