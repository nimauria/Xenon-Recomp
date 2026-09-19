#include "command_palette_feature.hpp"

#include "../application/application_feature.hpp"
#include "../community/community_feature.hpp"
#include "../diagnostics/diagnostics_feature.hpp"
#include "../launch/session/session_controller.hpp"
#include "../library/library_feature.hpp"
#include "../modules/modules_feature.hpp"
#include "../profiles/profiles_feature.hpp"
#include "../settings/settings_feature.hpp"
#include "../updates/update_feature.hpp"

#include <QRegularExpression>

#include <algorithm>

namespace xenon::launcher::frontend_backend {
namespace {

QVariantMap item(const QString& id, const QString& group, const QString& title,
                 const QString& subtitle, const QString& command_id,
                 const QString& target_id = {}, const QString& section_id = {},
                 const QString& keywords = {}, const QString& badge = {},
                 int priority = 0, bool quick = false) {
  return {{QStringLiteral("id"), id},
          {QStringLiteral("group"), group},
          {QStringLiteral("title"), title},
          {QStringLiteral("subtitle"), subtitle},
          {QStringLiteral("commandId"), command_id},
          {QStringLiteral("targetId"), target_id},
          {QStringLiteral("sectionId"), section_id},
          {QStringLiteral("keywords"), keywords},
          {QStringLiteral("badge"), badge},
          {QStringLiteral("priority"), priority},
          {QStringLiteral("quick"), quick}};
}

QString normalized(const QString& value) {
  auto result = value.simplified().toLower();
  result.replace(QRegularExpression(QStringLiteral("[^a-z0-9._+ -]+")), QStringLiteral(" "));
  return result.simplified();
}

int relevance(const QVariantMap& candidate, const QString& query) {
  const auto title = normalized(candidate.value(QStringLiteral("title")).toString());
  const auto subtitle = normalized(candidate.value(QStringLiteral("subtitle")).toString());
  const auto keywords = normalized(candidate.value(QStringLiteral("keywords")).toString());
  const auto target = normalized(candidate.value(QStringLiteral("targetId")).toString());
  const auto needle = normalized(query);
  if (needle.isEmpty()) return candidate.value(QStringLiteral("quick")).toBool()
                                   ? candidate.value(QStringLiteral("priority")).toInt()
                                   : -1;

  const auto tokens = needle.split(QLatin1Char(' '), Qt::SkipEmptyParts);
  int score = candidate.value(QStringLiteral("priority")).toInt();
  for (const auto& token : tokens) {
    bool matched = false;
    if (title.contains(token)) {
      score += title.startsWith(token) ? 120 : 80;
      matched = true;
    }
    if (subtitle.contains(token)) {
      score += 45;
      matched = true;
    }
    if (keywords.contains(token)) {
      score += 32;
      matched = true;
    }
    if (target.contains(token)) {
      score += 28;
      matched = true;
    }
    if (!matched) return -1;
  }

  if (title == needle) score += 600;
  else if (title.startsWith(needle)) score += 350;
  else if (title.contains(needle)) score += 180;
  return score;
}

bool containsProfile(const QVariantList& profiles, const QString& profile_id) {
  return std::any_of(profiles.cbegin(), profiles.cend(), [&](const QVariant& value) {
    return value.toMap().value(QStringLiteral("profileId")).toString() == profile_id;
  });
}

}  // namespace

CommandPaletteFeature::CommandPaletteFeature(ApplicationFeature& application,
                                             LibraryFeature& library,
                                             ModulesFeature& modules,
                                             ProfilesFeature& profiles,
                                             SettingsFeature& settings,
                                             UpdateFeature& updates,
                                             DiagnosticsFeature& diagnostics,
                                             CommunityFeature& community,
                                             SessionController& session,
                                             bool safe_mode,
                                             QObject* parent)
    : QObject(parent),
      application_(application),
      library_(library),
      modules_(modules),
      profiles_(profiles),
      settings_(settings),
      updates_(updates),
      diagnostics_(diagnostics),
      community_(community),
      session_(session),
      safe_mode_(safe_mode) {
  connect(&library_, &LibraryFeature::changed, this, &CommandPaletteFeature::changed);
  connect(&modules_, &ModulesFeature::changed, this, &CommandPaletteFeature::changed);
  connect(&modules_, &ModulesFeature::catalogChanged, this, &CommandPaletteFeature::changed);
  connect(&profiles_, &ProfilesFeature::changed, this, &CommandPaletteFeature::changed);
  connect(&settings_, &SettingsFeature::changed, this, [this](const QString&, const QVariant&) {
    emit changed();
  });
  connect(&updates_, &UpdateFeature::changed, this, &CommandPaletteFeature::changed);
  connect(&session_, &SessionController::changed, this, &CommandPaletteFeature::changed);
}

QVariantList CommandPaletteFeature::candidates() const {
  QVariantList result;

  if (!safe_mode_) {
    result.append(item(QStringLiteral("command:home"), QStringLiteral("Commands"),
                       QStringLiteral("Go to Home"), QStringLiteral("Open your Xenon activity dashboard"),
                       QStringLiteral("navigate.home"), {}, {},
                       QStringLiteral("home dashboard recent activity sessions playtime"), QStringLiteral("Ctrl+H"), 100, true));
    result.append(item(QStringLiteral("command:library"), QStringLiteral("Commands"),
                       QStringLiteral("Go to Library"), QStringLiteral("Open your Xenon game library"),
                       QStringLiteral("navigate.library"), {}, {},
                       QStringLiteral("games library titles"), QStringLiteral("Ctrl+1"), 95, true));
    result.append(item(QStringLiteral("command:modules"), QStringLiteral("Commands"),
                       QStringLiteral("Go to Modules"), QStringLiteral("Manage installed Xenon modules"),
                       QStringLiteral("navigate.modules"), {}, {},
                       QStringLiteral("modules plugins packages"), QStringLiteral("Ctrl+2"), 90, true));
    result.append(item(QStringLiteral("command:profiles"), QStringLiteral("Commands"),
                       QStringLiteral("Go to Profiles"), QStringLiteral("Manage launcher profiles"),
                       QStringLiteral("navigate.profiles"), {}, {},
                       QStringLiteral("profiles users configuration"), QStringLiteral("Ctrl+3"), 85, true));
    result.append(item(QStringLiteral("command:profile-create"), QStringLiteral("Commands"),
                       QStringLiteral("Create Profile"), QStringLiteral("Create a new Xenon launcher profile"),
                       QStringLiteral("profiles.create"), {}, QStringLiteral("create"),
                       QStringLiteral("new add profile user"), {}, 72, true));
    result.append(item(QStringLiteral("command:module-catalog"), QStringLiteral("Commands"),
                       QStringLiteral("Browse Module Catalog"),
                       QStringLiteral("Browse the official Xenon Modules registry"),
                       QStringLiteral("modules.catalog"), {}, QStringLiteral("catalog"),
                       QStringLiteral("registry discover install modules github"), {}, 70, true));
    result.append(item(QStringLiteral("command:launcher-update"), QStringLiteral("Commands"),
                       QStringLiteral("Check for Xenon Updates"),
                       QStringLiteral("Check GitHub for a newer launcher build"),
                       QStringLiteral("updates.launcher.check"), {}, {},
                       QStringLiteral("launcher update github release version"), {}, 66, true));
    result.append(item(QStringLiteral("command:module-updates"), QStringLiteral("Commands"),
                       QStringLiteral("Check Module Updates"),
                       QStringLiteral("Check all installed modules for updates"),
                       QStringLiteral("updates.modules.check"), {}, {},
                       QStringLiteral("module updates registry releases"), {}, 64, true));
  }

  result.append(item(QStringLiteral("command:settings"), QStringLiteral("Commands"),
                     QStringLiteral("Go to Settings"), QStringLiteral("Open Xenon launcher settings"),
                     QStringLiteral("navigate.settings"), {}, {},
                     QStringLiteral("preferences configuration options"), QStringLiteral("Ctrl+,"), 88, true));
  result.append(item(QStringLiteral("command:support-bundle"), QStringLiteral("Commands"),
                     QStringLiteral("Create Support Bundle"),
                     QStringLiteral("Export privacy-sanitized diagnostics for support"),
                     QStringLiteral("support.bundle"), {}, {},
                     QStringLiteral("diagnostics logs support discord bug report zip"), {}, 62, true));
  result.append(item(QStringLiteral("command:discord"), QStringLiteral("Commands"),
                     QStringLiteral("Join Xenon Discord"),
                     QStringLiteral("Open the Xenon community and support server"),
                     QStringLiteral("community.discord"), {}, {},
                     QStringLiteral("discord community help support"), {}, 58, true));

  if (session_.active()) {
    const auto current = session_.currentSession();
    const auto title = current.value(QStringLiteral("title"), QStringLiteral("current game")).toString();
    result.prepend(item(QStringLiteral("command:stop-session"), QStringLiteral("Session"),
                        QStringLiteral("Stop %1").arg(title),
                        QStringLiteral("Stop or cancel the current Xenon session"),
                        QStringLiteral("session.stop"), {}, {},
                        QStringLiteral("stop cancel session game running"), QStringLiteral("ACTIVE"), 130, true));
  }

  for (const auto& category_value : settings_.categories()) {
    const auto category = category_value.toMap();
    if (!application_.featureEnabled(category.value(QStringLiteral("feature")).toString())) continue;
    const auto id = category.value(QStringLiteral("id")).toString();
    const auto name = category.value(QStringLiteral("name")).toString();
    result.append(item(QStringLiteral("setting:") + id, QStringLiteral("Settings"), name,
                       QStringLiteral("Settings • %1").arg(name), QStringLiteral("navigate.settings"), id, {},
                       category.value(QStringLiteral("keywords")).toString() + QLatin1Char(' ') + id,
                       QStringLiteral("SETTING"), 30));
  }

  if (safe_mode_) return result;

  for (const auto& value : library_.entries()) {
    const auto game = value.toMap();
    const auto game_id = game.value(QStringLiteral("gameId")).toString();
    const auto title = game.value(QStringLiteral("title")).toString();
    if (game_id.isEmpty() || title.isEmpty()) continue;
    const auto module_name = game.value(QStringLiteral("moduleName")).toString();
    result.append(item(QStringLiteral("game:") + game_id, QStringLiteral("Games"), title,
                       module_name.isEmpty() ? QStringLiteral("Library")
                                             : QStringLiteral("Library • %1").arg(module_name),
                       QStringLiteral("navigate.game"), game_id, {},
                       QStringLiteral("%1 %2 %3 %4 %5")
                           .arg(game_id,
                                game.value(QStringLiteral("moduleId")).toString(),
                                module_name,
                                game.value(QStringLiteral("tags")).toString(),
                                game.value(QStringLiteral("status")).toString()),
                       QStringLiteral("GAME"), 46));
  }

  for (const auto& value : modules_.entries()) {
    const auto module = value.toMap();
    const auto module_id = module.value(QStringLiteral("moduleId")).toString();
    const auto name = module.value(QStringLiteral("moduleName")).toString();
    if (module_id.isEmpty() || name.isEmpty()) continue;
    const auto version = module.value(QStringLiteral("version")).toString();
    result.append(item(QStringLiteral("module:") + module_id, QStringLiteral("Modules"), name,
                       version.isEmpty() ? QStringLiteral("Installed module")
                                         : QStringLiteral("Module • Version %1").arg(version),
                       QStringLiteral("navigate.module"), module_id, {},
                       QStringLiteral("%1 %2 %3 %4")
                           .arg(module_id,
                                module.value(QStringLiteral("moduleType")).toString(),
                                module.value(QStringLiteral("description")).toString(),
                                module.value(QStringLiteral("status")).toString()),
                       module.value(QStringLiteral("active")).toBool() ? QStringLiteral("ENABLED")
                                                                        : QStringLiteral("MODULE"),
                       44));
  }

  const auto profiles = profiles_.entries();
  for (const auto& value : profiles) {
    const auto profile = value.toMap();
    const auto profile_id = profile.value(QStringLiteral("profileId")).toString();
    const auto name = profile.value(QStringLiteral("profileName")).toString();
    if (profile_id.isEmpty() || name.isEmpty()) continue;
    result.append(item(QStringLiteral("profile:") + profile_id, QStringLiteral("Profiles"), name,
                       profile.value(QStringLiteral("active")).toBool()
                           ? QStringLiteral("Profiles • Active profile")
                           : QStringLiteral("Profiles • Launcher profile"),
                       QStringLiteral("navigate.profile"), profile_id, {},
                       QStringLiteral("%1 %2 %3")
                           .arg(profile_id,
                                profile.value(QStringLiteral("description")).toString(),
                                profile.value(QStringLiteral("region")).toString()),
                       profile.value(QStringLiteral("active")).toBool() ? QStringLiteral("ACTIVE")
                                                                         : QStringLiteral("PROFILE"),
                       40));
  }

  return result;
}

QVariantList CommandPaletteFeature::search(const QString& query, int limit) const {
  struct RankedItem {
    int score = 0;
    QVariantMap value;
  };

  QList<RankedItem> ranked;
  const auto all = candidates();
  ranked.reserve(all.size());
  for (const auto& value : all) {
    auto candidate = value.toMap();
    const auto score = relevance(candidate, query);
    if (score < 0) continue;
    candidate.insert(QStringLiteral("score"), score);
    ranked.push_back({score, candidate});
  }

  std::stable_sort(ranked.begin(), ranked.end(), [](const RankedItem& left, const RankedItem& right) {
    if (left.score != right.score) return left.score > right.score;
    const auto left_group = left.value.value(QStringLiteral("group")).toString();
    const auto right_group = right.value.value(QStringLiteral("group")).toString();
    if (left_group != right_group) return left_group < right_group;
    return left.value.value(QStringLiteral("title")).toString().localeAwareCompare(
               right.value.value(QStringLiteral("title")).toString()) < 0;
  });

  const auto bounded_limit = qBound(1, limit, 50);
  QVariantList result;
  result.reserve(qMin(bounded_limit, static_cast<int>(ranked.size())));
  for (int i = 0; i < ranked.size() && i < bounded_limit; ++i) result.append(ranked.at(i).value);
  return result;
}

bool CommandPaletteFeature::settingCategoryAvailable(const QString& category_id) const {
  for (const auto& value : settings_.categories()) {
    const auto category = value.toMap();
    if (category.value(QStringLiteral("id")).toString() != category_id) continue;
    return application_.featureEnabled(category.value(QStringLiteral("feature")).toString());
  }
  return false;
}

ServiceResult CommandPaletteFeature::execute(const QString& command_id,
                                             const QString& target_id,
                                             const QString& section_id) {
  const auto command = command_id.trimmed();
  if (command == QStringLiteral("navigate.settings")) {
    if (!target_id.isEmpty() && !settingCategoryAvailable(target_id)) {
      return ServiceResult::failure(QStringLiteral("Command palette"),
                                    QStringLiteral("That settings category is not available in this build."));
    }
    emit navigationRequested(3, target_id, section_id);
    return ServiceResult::success();
  }
  if (command == QStringLiteral("navigate.home")) {
    if (safe_mode_) return ServiceResult::failure(QStringLiteral("Safe Mode"), QStringLiteral("Home activity is not loaded in Safe Mode."));
    emit navigationRequested(4, {}, {});
    return ServiceResult::success();
  }
  if (command == QStringLiteral("navigate.library")) {
    if (safe_mode_) return ServiceResult::failure(QStringLiteral("Safe Mode"), QStringLiteral("Library is not loaded in Safe Mode."));
    emit navigationRequested(0, {}, {});
    return ServiceResult::success();
  }
  if (command == QStringLiteral("navigate.modules")) {
    if (safe_mode_) return ServiceResult::failure(QStringLiteral("Safe Mode"), QStringLiteral("Modules are not loaded in Safe Mode."));
    emit navigationRequested(1, {}, {});
    return ServiceResult::success();
  }
  if (command == QStringLiteral("navigate.profiles")) {
    if (safe_mode_) return ServiceResult::failure(QStringLiteral("Safe Mode"), QStringLiteral("Profiles are not loaded in Safe Mode."));
    emit navigationRequested(2, {}, {});
    return ServiceResult::success();
  }
  if (command == QStringLiteral("navigate.game")) {
    if (safe_mode_ || library_.entry(target_id).isEmpty()) {
      return ServiceResult::failure(QStringLiteral("Command palette"), QStringLiteral("That game is no longer available."));
    }
    emit navigationRequested(0, target_id, {});
    return ServiceResult::success();
  }
  if (command == QStringLiteral("navigate.module")) {
    if (safe_mode_ || modules_.module(target_id).isEmpty()) {
      return ServiceResult::failure(QStringLiteral("Command palette"), QStringLiteral("That module is no longer available."));
    }
    emit navigationRequested(1, target_id, {});
    return ServiceResult::success();
  }
  if (command == QStringLiteral("navigate.profile")) {
    if (safe_mode_ || !containsProfile(profiles_.entries(), target_id)) {
      return ServiceResult::failure(QStringLiteral("Command palette"), QStringLiteral("That profile is no longer available."));
    }
    emit navigationRequested(2, target_id, {});
    return ServiceResult::success();
  }
  if (command == QStringLiteral("profiles.create")) {
    if (safe_mode_) return ServiceResult::failure(QStringLiteral("Safe Mode"), QStringLiteral("Profile editing is unavailable in Safe Mode."));
    emit navigationRequested(2, {}, QStringLiteral("create"));
    return ServiceResult::success();
  }
  if (command == QStringLiteral("modules.catalog")) {
    if (safe_mode_) return ServiceResult::failure(QStringLiteral("Safe Mode"), QStringLiteral("The module catalogue is unavailable in Safe Mode."));
    emit navigationRequested(1, {}, QStringLiteral("catalog"));
    return ServiceResult::success();
  }
  if (command == QStringLiteral("updates.launcher.check")) {
    if (safe_mode_) return ServiceResult::failure(QStringLiteral("Safe Mode"), QStringLiteral("Automatic update services are suspended in Safe Mode."));
    return updates_.checkLauncher(true);
  }
  if (command == QStringLiteral("updates.modules.check")) {
    if (safe_mode_) return ServiceResult::failure(QStringLiteral("Safe Mode"), QStringLiteral("Module update services are suspended in Safe Mode."));
    return modules_.checkAllUpdates();
  }
  if (command == QStringLiteral("support.bundle")) return diagnostics_.createSupportBundle();
  if (command == QStringLiteral("community.discord")) return community_.openDiscord();
  if (command == QStringLiteral("session.stop")) return session_.stop();

  return ServiceResult::failure(QStringLiteral("Command palette"),
                                QStringLiteral("The selected command is not recognised by this launcher build."));
}

}  // namespace xenon::launcher::frontend_backend
