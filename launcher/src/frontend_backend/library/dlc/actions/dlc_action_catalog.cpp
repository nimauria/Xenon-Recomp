#include "dlc_action_catalog.hpp"

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

QVariantList DlcActionCatalog::actions(const QVariantMap& dlc, bool test_mode) {
  if (dlc.isEmpty()) return {};
  const auto installed = dlc.value(QStringLiteral("installed")).toBool();
  if (!installed) {
    return {action(QStringLiteral("import"), QStringLiteral("Import local DLC…"), QStringLiteral("+"))};
  }

  const auto can_open = !test_mode && dlc.value(QStringLiteral("canOpen"), true).toBool();
  const auto can_verify = dlc.value(QStringLiteral("canVerify"), true).toBool();
  const auto can_remove = dlc.value(QStringLiteral("canRemove"), true).toBool();
  return {
      action(QStringLiteral("open"), QStringLiteral("Open DLC folder"), QStringLiteral("▣"), can_open,
             false, false, can_open ? QString{} : QStringLiteral("This DLC entry has no real local folder in fixture mode.")),
      action(QStringLiteral("verify"), QStringLiteral("Verify DLC"), QStringLiteral("✓"), can_verify),
      action(QStringLiteral("remove"), QStringLiteral("Remove DLC"), QStringLiteral("×"), can_remove,
             true, true),
  };
}

QVariantList DlcActionCatalog::backgroundActions(bool has_catalogue) {
  return {
      action(QStringLiteral("import"), QStringLiteral("Import DLC…"), QStringLiteral("+"), has_catalogue,
             false, false, has_catalogue ? QString{} : QStringLiteral("The selected module does not declare a DLC catalogue.")),
  };
}

}  // namespace xenon::launcher::frontend_backend
