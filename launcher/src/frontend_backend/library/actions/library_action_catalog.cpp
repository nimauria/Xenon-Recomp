#include "library_action_catalog.hpp"

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

QString fixtureReason(bool test_mode) {
  return test_mode ? QStringLiteral("Fixture library entries do not have a real local folder.") : QString{};
}

}  // namespace

QVariantList LibraryActionCatalog::gameActions(const QVariantMap& game, bool test_mode) {
  if (game.isEmpty()) return {};
  const auto ready = game.value(QStringLiteral("ready")).toBool();
  const auto title = game.value(QStringLiteral("title"), QStringLiteral("game")).toString();
  const auto has_module = !game.value(QStringLiteral("moduleId")).toString().trimmed().isEmpty();
  const auto module_available = game.value(QStringLiteral("moduleInstalled"), has_module).toBool();
  const auto module_active = game.value(QStringLiteral("moduleActive"), module_available).toBool();
  const auto content_openable = !test_mode && game.value(QStringLiteral("contentExists"), true).toBool();
  const auto managed_paths = !test_mode;
  const auto content_reason = test_mode ? fixtureReason(true)
                                        : QStringLiteral("The registered game content is missing.");

  QVariantList result;
  if (module_available && !module_active) {
    result.append(action(QStringLiteral("enableModule"),
                         QStringLiteral("Enable assigned module"), QStringLiteral("▶")));
  }
  result.append(action(QStringLiteral("play"), QStringLiteral("Play"), QStringLiteral("▶"), ready,
                       module_available && !module_active, false,
                       ready ? QString{} : QStringLiteral("This game is not currently launch-ready.")));
  result.append(action(QStringLiteral("properties"), QStringLiteral("Game properties"), QStringLiteral("ⓘ")));
  result.append(action(QStringLiteral("refreshMetadata"),
                       QStringLiteral("Refresh GitHub metadata"), QStringLiteral("↻"),
                       has_module, false, false,
                       has_module ? QString{} : QStringLiteral("This game does not have an assigned module.")));
  result.append(action(QStringLiteral("moduleSettings"), QStringLiteral("Module settings"), QStringLiteral("◇"), module_available,
                       false, false, module_available ? QString{} : QStringLiteral("The assigned game module is not available.")));
  result.append(action(QStringLiteral("browse"), QStringLiteral("Browse game files"), QStringLiteral("▣"), content_openable,
                       true, false, content_openable ? QString{} : content_reason));
  result.append(action(QStringLiteral("saves"), QStringLiteral("Open save data"), QStringLiteral("▤"), managed_paths,
                       false, false, fixtureReason(test_mode)));
  result.append(action(QStringLiteral("module"), QStringLiteral("Open module folder"), QStringLiteral("◇"), !test_mode && module_available,
                       false, false, test_mode ? fixtureReason(true)
                                               : module_available ? QString{} : QStringLiteral("The assigned game module is not available.")));
  result.append(action(QStringLiteral("verify"), QStringLiteral("Verify imported content"), QStringLiteral("✓"), true, true));
  result.append(action(QStringLiteral("copyId"), QStringLiteral("Copy game ID"), QStringLiteral("#")));
  result.append(action(QStringLiteral("remove"), QStringLiteral("Remove “%1” from Library").arg(title), QStringLiteral("×"),
                       true, true, true));
  return result;
}

QVariantList LibraryActionCatalog::manageActions(const QVariantMap& game, bool test_mode) {
  if (game.isEmpty()) return {};
  const auto has_module = !game.value(QStringLiteral("moduleId")).toString().trimmed().isEmpty();
  const auto module_available = game.value(QStringLiteral("moduleInstalled"), has_module).toBool();
  const auto content_openable = !test_mode && game.value(QStringLiteral("contentExists"), true).toBool();
  const auto managed_paths = !test_mode;
  const auto content_reason = test_mode ? fixtureReason(true)
                                        : QStringLiteral("The registered game content is missing.");
  return {
      action(QStringLiteral("browse"), QStringLiteral("Browse game files"), QStringLiteral("▣"), content_openable,
             false, false, content_openable ? QString{} : content_reason),
      action(QStringLiteral("saves"), QStringLiteral("Open save data"), QStringLiteral("▤"), managed_paths,
             false, false, fixtureReason(test_mode)),
      action(QStringLiteral("module"), QStringLiteral("Open module folder"), QStringLiteral("◇"), !test_mode && module_available,
             false, false, test_mode ? fixtureReason(true)
                                     : module_available ? QString{} : QStringLiteral("The assigned game module is not available.")),
      action(QStringLiteral("verify"), QStringLiteral("Verify imported content"), QStringLiteral("✓"), true, true),
  };
}

QVariantList LibraryActionCatalog::backgroundActions() {
  return {
      action(QStringLiteral("add"), QStringLiteral("Add game…"), QStringLiteral("+")),
  };
}

}  // namespace xenon::launcher::frontend_backend
