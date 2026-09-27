#include "profile_action_catalog.hpp"

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

QVariantList ProfileActionCatalog::actions(const QVariantMap& profile, int profile_count) {
  if (profile.isEmpty()) return {};
  const auto active = profile.value(QStringLiteral("active")).toBool();
  const auto has_avatar = !profile.value(QStringLiteral("avatarPath")).toString().trimmed().isEmpty();
  const auto can_delete = !active && profile_count > 1;
  const auto storage_openable = profile.value(QStringLiteral("storageOpenable"), true).toBool();

  QVariantList result;
  result.append(action(QStringLiteral("activate"), active ? QStringLiteral("Active profile") : QStringLiteral("Set active"),
                       QStringLiteral("\u2713"), !active, false, false,
                       active ? QStringLiteral("This profile is already active.") : QString{}));
  result.append(action(QStringLiteral("edit"), QStringLiteral("Edit profile"), QStringLiteral("\u270E")));
  result.append(action(QStringLiteral("runtime"), QStringLiteral("Profile runtime settings"), QStringLiteral("\u2699")));
  result.append(action(QStringLiteral("paths"), QStringLiteral("Profile content locations"), QStringLiteral("\u2302")));
  result.append(action(QStringLiteral("duplicate"), QStringLiteral("Duplicate profile"), QStringLiteral("\u29C9")));
  result.append(action(QStringLiteral("export"), QStringLiteral("Export profile"), QStringLiteral("\u21E7"), true, true));
  result.append(action(QStringLiteral("copyId"), QStringLiteral("Copy profile ID"), QStringLiteral("#")));
  result.append(action(QStringLiteral("openStorage"), QStringLiteral("Open profile storage"), QStringLiteral("\u2197"), storage_openable, false, false,
                       storage_openable ? QString{} : QStringLiteral("Fixture profiles do not have a real storage folder.")));
  if (has_avatar) {
    result.append(action(QStringLiteral("removeAvatar"), QStringLiteral("Remove profile image"), QStringLiteral("\u2212")));
  }
  result.append(action(QStringLiteral("delete"), QStringLiteral("Delete profile"), QStringLiteral("\u00D7"), can_delete,
                       true, true,
                       active ? QStringLiteral("Activate another profile before deleting this one.")
                              : profile_count <= 1 ? QStringLiteral("At least one profile must remain.") : QString{}));
  return result;
}

QVariantList ProfileActionCatalog::backgroundActions() {
  return {
      action(QStringLiteral("create"), QStringLiteral("Create profile"), QStringLiteral("+")),
      action(QStringLiteral("import"), QStringLiteral("Import profile\u2026"), QStringLiteral("\u21E9")),
  };
}

}  // namespace xenon::launcher::frontend_backend
