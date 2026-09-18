#include "module_action_catalog.hpp"

namespace xenon::launcher::frontend_backend {
namespace {

QVariantMap action(const QString& id, const QString& label, const QString& icon,
                   bool enabled = true, bool separator_before = false,
                   bool destructive = false, const QString& disabled_reason = {}) {
  QVariantMap value{{QStringLiteral("id"), id},
                    {QStringLiteral("label"), label},
                    {QStringLiteral("icon"), icon},
                    {QStringLiteral("enabled"), enabled},
                    {QStringLiteral("separatorBefore"), separator_before},
                    {QStringLiteral("destructive"), destructive}};
  if (!disabled_reason.isEmpty()) value.insert(QStringLiteral("disabledReason"), disabled_reason);
  return value;
}

}  // namespace

QVariantList ModuleActionCatalog::actions(const QVariantMap& module,
                                          const QVariantMap& update_state,
                                          bool has_settings) {
  if (module.isEmpty()) return {};

  const auto active = module.value(QStringLiteral("active")).toBool();
  const auto path = module.value(QStringLiteral("path")).toString().trimmed();
  const auto repository = module.value(QStringLiteral("repositoryUrl")).toString().trimmed();
  const auto catalog_known = module.value(QStringLiteral("catalogKnown")).toBool();
  const auto package_supported = module.value(QStringLiteral("packageSupported")).toBool();
  const auto update_status = update_state.value(QStringLiteral("status"), QStringLiteral("idle")).toString();
  const auto can_download = update_state.value(QStringLiteral("canDownload")).toBool();
  const auto can_install = update_state.value(QStringLiteral("canInstall")).toBool();
  const auto checking = update_status == QStringLiteral("checking");
  const auto downloading = update_status == QStringLiteral("downloading");

  QVariantList result;
  result.append(action(QStringLiteral("toggleEnabled"),
                       active ? QStringLiteral("Disable module") : QStringLiteral("Enable module"),
                       active ? QStringLiteral("\u23F8") : QStringLiteral("\u25B6")));
  result.append(action(QStringLiteral("settings"), QStringLiteral("Module settings"),
                       QStringLiteral("\u2699"), has_settings, false, false,
                       has_settings ? QString{} : QStringLiteral("This module does not declare launcher settings.")));

  if (can_install) {
    result.append(action(QStringLiteral("installUpdate"), QStringLiteral("Install verified update"),
                         QStringLiteral("\u21E9"), true, true));
  } else if (can_download) {
    result.append(action(QStringLiteral("downloadUpdate"), QStringLiteral("Download update"),
                         QStringLiteral("\u21E9"), true, true));
  } else {
    const auto update_enabled = catalog_known && package_supported && !checking && !downloading;
    QString disabled_reason;
    if (!catalog_known) {
      disabled_reason = QStringLiteral("This local module is not listed in the official Xenon module catalog.");
    } else if (!package_supported) {
      disabled_reason = QStringLiteral("The catalog does not define a package for this operating system and architecture.");
    } else if (checking || downloading) {
      disabled_reason = QStringLiteral("An update operation is already in progress.");
    }
    result.append(action(QStringLiteral("checkUpdate"),
                         checking ? QStringLiteral("Checking for updates…")
                                  : downloading ? QStringLiteral("Downloading update…")
                                                : QStringLiteral("Check for updates"),
                         QStringLiteral("\u21BB"), update_enabled, true, false, disabled_reason));
  }

  result.append(action(QStringLiteral("verify"), QStringLiteral("Verify module"), QStringLiteral("\u2713")));
  result.append(action(QStringLiteral("openFolder"), QStringLiteral("Open module folder"),
                       QStringLiteral("\u2197"), !path.isEmpty(), false, false,
                       path.isEmpty() ? QStringLiteral("This module does not have a local module directory.") : QString{}));
  result.append(action(QStringLiteral("openRepository"), QStringLiteral("Open repository"),
                       QStringLiteral("\u2197"), !repository.isEmpty(), false, false,
                       repository.isEmpty() ? QStringLiteral("No repository is associated with this module in the catalog.") : QString{}));
  result.append(action(QStringLiteral("copyId"), QStringLiteral("Copy module ID"), QStringLiteral("#")));
  result.append(action(QStringLiteral("remove"), QStringLiteral("Remove module"), QStringLiteral("\u00D7"),
                       !path.isEmpty(), true, true,
                       path.isEmpty() ? QStringLiteral("Fixture modules cannot be removed from disk.") : QString{}));
  return result;
}

QVariantList ModuleActionCatalog::pageActions() {
  return {
      action(QStringLiteral("browseCatalog"), QStringLiteral("Browse module catalog"), QStringLiteral("\u25C7")),
      action(QStringLiteral("importLocal"), QStringLiteral("Import module package"), QStringLiteral("+")),
      action(QStringLiteral("importFolder"), QStringLiteral("Import unpacked module folder"), QStringLiteral("▣")),
      action(QStringLiteral("refreshInstalled"), QStringLiteral("Refresh installed modules"), QStringLiteral("\u21BB"), true, true),
      action(QStringLiteral("refreshCatalog"), QStringLiteral("Refresh GitHub catalog"), QStringLiteral("\u21BB")),
      action(QStringLiteral("checkAllUpdates"), QStringLiteral("Check all module updates"), QStringLiteral("\u21E9")),
  };
}

}  // namespace xenon::launcher::frontend_backend
