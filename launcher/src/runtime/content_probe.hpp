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

#if defined(XENON_LAUNCHER_HAS_FILESYSTEM)
// Thin Qt adapter over Xenon::Filesystem's host-neutral content probe. Module
// matching remains a launcher responsibility because installed manifests are
// launcher state, while XEX parsing and source classification remain in Xenon.
class XenonContentProbe final : public IContentProbe {
 public:
  [[nodiscard]] bool available() const noexcept override { return true; }
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
using DefaultContentProbe = XenonContentProbe;
#else
using DefaultContentProbe = UnavailableContentProbe;
#endif

}  // namespace xenon::launcher
