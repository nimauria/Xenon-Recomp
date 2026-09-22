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

  // Identifies the Xbox content itself (title/media identity, XEX version,
  // disc info, source type/paths) from `source`. A compatible installed
  // module is looked up as a best-effort convenience: when exactly one
  // module matches, its "moduleId"/"moduleName"/"moduleVersion" and
  // manifest-declared display fields are included; when zero modules match,
  // identification still succeeds with those fields left empty ("moduleId"
  // is empty, "ready" is false) so a legally-owned disc can be added to the
  // library before its game module exists - module resolution can happen
  // later (installing a module, or matchModule() re-run at Play time). Only
  // genuine ambiguity (more than one installed module claims the same
  // identity) is a hard failure, since guessing between them would be wrong.
  [[nodiscard]] virtual ServiceResult identifyGame(
      const QUrl& source, const QVariantList& module_candidates) const = 0;

  // Re-runs installed-module matching for an ALREADY-known title/media
  // identity (e.g. a library entry whose content was identified before any
  // compatible module was installed) without touching the original content
  // source again. On success, `data` is a QVariantMap with "moduleId",
  // "moduleName", "moduleVersion" (and any manifest-declared display
  // fields identifyGame() would have included). Fails when zero or more than
  // one installed module matches.
  [[nodiscard]] virtual ServiceResult matchModule(
      const QString& title_id, const QString& media_id, const QString& xex_version,
      int disc_number, const QVariantList& module_candidates) const = 0;

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
  [[nodiscard]] ServiceResult matchModule(
      const QString& title_id, const QString& media_id, const QString& xex_version,
      int disc_number, const QVariantList& module_candidates) const override;
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
  [[nodiscard]] ServiceResult matchModule(
      const QString& title_id, const QString& media_id, const QString& xex_version,
      int disc_number, const QVariantList& module_candidates) const override;
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
