#include "profiles_feature.hpp"

#include "actions/profile_action_catalog.hpp"
#include "runtime/profile_runtime_catalog.hpp"
#include "../runtime/runtime_feature.hpp"
#include "../settings/settings_feature.hpp"
#include "../../services/library_service.hpp"
#include "../../services/module_service.hpp"
#include "../../services/path_service.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>

namespace xenon::launcher::frontend_backend {
namespace {
QString nowIso() { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }

QString limited(QString value, int maximum) {
  value = value.trimmed();
  if (value.size() > maximum) value.truncate(maximum);
  return value;
}

QUrl normalizedDestination(QUrl destination) {
  if (!destination.isLocalFile()) return destination;
  auto path = destination.toLocalFile();
  if (QFileInfo{path}.suffix().isEmpty()) path += QStringLiteral(".xenonprofile");
  return QUrl::fromLocalFile(path);
}

bool containsValue(const QStringList& values, const QString& value) {
  return values.contains(value.trimmed(), Qt::CaseInsensitive);
}
}  // namespace

ProfilesFeature::ProfilesFeature(ProfileService& profiles, PathService& paths, LibraryService& library,
                                 ModuleService& modules, SettingsFeature& settings,
                                 RuntimeFeature& runtime, bool test_mode, QObject* parent)
    : QObject(parent), profiles_(profiles), paths_(paths), library_(library), modules_(modules),
      settings_(settings), runtime_(runtime), test_mode_(test_mode) {
  connect(&profiles_, &ProfileService::changed, this, [this]() {
    if (!test_mode_) emit changed();
  });
  connect(&settings_, &SettingsFeature::changed, this, [this](const QString& key, const QVariant&) {
    if (ProfileRuntimeCatalog::keys().contains(key) || key.startsWith(QStringLiteral("paths/"))) emit changed();
  });
}

void ProfilesFeature::initialize(const QString& primary_name) {
  primary_name_ = primary_name.trimmed().isEmpty() ? QStringLiteral("Nimauria") : primary_name.trimmed();
  if (test_mode_) {
    resetFixtures(primary_name_);
  } else {
    profiles_.initialize(primary_name_);
  }
}

void ProfilesFeature::refreshDerivedState() { emit changed(); }

void ProfilesFeature::reload() {
  if (test_mode_) return;
  profiles_.initialize(activeProfile().value(QStringLiteral("profileName"), primary_name_).toString());
  emit changed();
}

QVariantList ProfilesFeature::sourceEntries() const {
  return test_mode_ ? fixture_profiles_ : profiles_.profiles();
}

int ProfilesFeature::saveSetCount(const QString& path) const {
  if (test_mode_ || path.trimmed().isEmpty()) return 0;
  const QDir directory{path};
  if (!directory.exists()) return 0;
  return directory.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size();
}

QVariantMap ProfilesFeature::resolvedRuntimeSettings(const QVariantMap& profile) const {
  return ProfileRuntimeCatalog::resolved(profile, settings_);
}

QVariantMap ProfilesFeature::decorated(QVariantMap item) const {
  const auto game_override = item.value(QStringLiteral("gamePath")).toString().trimmed();
  const auto save_override = item.value(QStringLiteral("savePath")).toString().trimmed();
  const auto screenshot_override = item.value(QStringLiteral("screenshotPath")).toString().trimmed();
  const auto effective_game = game_override.isEmpty() ? paths_.configuredPath(QStringLiteral("games")) : game_override;
  const auto effective_save = save_override.isEmpty() ? paths_.configuredPath(QStringLiteral("saves")) : save_override;
  const auto effective_screenshots = screenshot_override.isEmpty() ? paths_.configuredPath(QStringLiteral("screenshots")) : screenshot_override;

  item.insert(QStringLiteral("effectiveGamePath"), effective_game);
  item.insert(QStringLiteral("effectiveSavePath"), effective_save);
  item.insert(QStringLiteral("effectiveScreenshotPath"), effective_screenshots);
  item.insert(QStringLiteral("storagePath"), test_mode_
      ? QStringLiteral("TEST://Profiles/%1").arg(item.value(QStringLiteral("profileId")).toString())
      : QDir{paths_.configuredPath(QStringLiteral("profiles"))}.filePath(item.value(QStringLiteral("profileId")).toString()));

  item.insert(QStringLiteral("storageOpenable"), !test_mode_);

  // QML Image.source expects a URL. Older/test profile data may contain a
  // native absolute path, so normalize it at the frontend-backend boundary.
  const auto avatar = item.value(QStringLiteral("avatarPath")).toString().trimmed();
  if (!avatar.isEmpty()) {
    const QUrl avatar_url{avatar};
    if (QDir::isAbsolutePath(avatar) && !avatar_url.isLocalFile()) {
      item.insert(QStringLiteral("avatarPath"), QUrl::fromLocalFile(avatar).toString());
    }
  }

  const auto runtime_overrides = item.value(QStringLiteral("runtimeOverrides")).toMap();
  item.insert(QStringLiteral("runtimeOverrideCount"), runtime_overrides.size());
  item.insert(QStringLiteral("effectiveRuntimeSettings"), resolvedRuntimeSettings(item));
  item.insert(QStringLiteral("pathOverrideCount"),
              (game_override.isEmpty() ? 0 : 1) + (save_override.isEmpty() ? 0 : 1) +
                  (screenshot_override.isEmpty() ? 0 : 1));

  if (!test_mode_) {
    item.insert(QStringLiteral("games"), library_.entries().size());
    item.insert(QStringLiteral("modules"), modules_.modules().size());
    item.insert(QStringLiteral("saveSets"), saveSetCount(effective_save));
  }
  return item;
}

QVariantList ProfilesFeature::entries() const {
  QVariantList result;
  const auto source = sourceEntries();
  result.reserve(source.size());
  for (const auto& value : source) result.append(decorated(value.toMap()));
  return result;
}

QVariantMap ProfilesFeature::profile(int index) const {
  const auto source = sourceEntries();
  if (source.isEmpty()) return {};
  const auto safe = qBound(0, index, static_cast<int>(source.size()) - 1);
  return decorated(source.at(safe).toMap());
}

QVariantMap ProfilesFeature::activeProfile() const { return profile(activeIndex()); }
int ProfilesFeature::activeIndex() const noexcept { return test_mode_ ? fixture_active_index_ : profiles_.activeIndex(); }

bool ProfilesFeature::nameAvailable(const QString& name, int exclude_index) const {
  if (!test_mode_) return profiles_.nameAvailable(name, exclude_index);
  const auto needle = limited(name, profileNameLimit()).toLower();
  if (needle.isEmpty()) return false;
  for (qsizetype i = 0; i < fixture_profiles_.size(); ++i) {
    if (static_cast<int>(i) == exclude_index) continue;
    if (fixture_profiles_.at(i).toMap().value(QStringLiteral("profileName")).toString().trimmed().toLower() == needle)
      return false;
  }
  return true;
}

QVariantList ProfilesFeature::actions(int index) const {
  const auto source = sourceEntries();
  if (index < 0 || index >= static_cast<int>(source.size())) return {};
  return ProfileActionCatalog::actions(decorated(source.at(index).toMap()), static_cast<int>(source.size()));
}

QVariantList ProfilesFeature::backgroundActions() const {
  return ProfileActionCatalog::backgroundActions();
}

QVariantList ProfilesFeature::runtimeDefinitions() const {
  return ProfileRuntimeCatalog::definitions(settings_, runtime_, test_mode_);
}

QVariantMap ProfilesFeature::runtimeDefaults() const {
  return ProfileRuntimeCatalog::defaults(settings_);
}

QString ProfilesFeature::storagePath(int index) const {
  const auto item = profile(index);
  if (item.isEmpty()) return {};
  return item.value(QStringLiteral("storagePath")).toString();
}

ServiceResult ProfilesFeature::ensureStorage(int index) const {
  const auto path = storagePath(index);
  if (path.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Profile storage"),
                                  QStringLiteral("The selected profile no longer exists."));
  }
  if (test_mode_) return ServiceResult::success({}, {}, path);
  if (!paths_.ensureDirectory(path)) {
    return ServiceResult::failure(QStringLiteral("Profile storage"),
                                  QStringLiteral("Xenon could not create the profile storage folder."));
  }
  return ServiceResult::success({}, {}, path);
}

QString ProfilesFeature::newProfileId() const {
  if (!test_mode_) return profiles_.newProfileId();
  return QStringLiteral("profile-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

QVariantMap ProfilesFeature::fixtureProfile(const QString& name, const QString& description,
                                            bool active, const QString& id) const {
  QVariantMap item;
  item.insert(QStringLiteral("profileId"), id);
  item.insert(QStringLiteral("profileName"), limited(name, profileNameLimit()));
  item.insert(QStringLiteral("description"), limited(description, descriptionLimit()));
  item.insert(QStringLiteral("active"), active);
  item.insert(QStringLiteral("games"), 0);
  item.insert(QStringLiteral("avatarPath"), QString{});
  item.insert(QStringLiteral("modules"), 0);
  item.insert(QStringLiteral("saveSets"), 0);
  item.insert(QStringLiteral("gamePath"), QString{});
  item.insert(QStringLiteral("savePath"), QString{});
  item.insert(QStringLiteral("screenshotPath"), QString{});
  item.insert(QStringLiteral("region"), QStringLiteral("Auto (Global)"));
  item.insert(QStringLiteral("startupPage"), QStringLiteral("Launcher default"));
  item.insert(QStringLiteral("offline"), true);
  item.insert(QStringLiteral("isolatedSettings"), false);
  item.insert(QStringLiteral("runtimeOverrides"), QVariantMap{});
  item.insert(QStringLiteral("createdAt"), nowIso());
  item.insert(QStringLiteral("lastUsedAt"), active ? nowIso() : QString{});
  item.insert(QStringLiteral("lastUsed"), active ? QStringLiteral("Current session") : QStringLiteral("Not used yet"));
  return item;
}

void ProfilesFeature::resetFixtures(const QString& primary_name) {
  fixture_profiles_.clear();
  fixture_profiles_.append(fixtureProfile(primary_name, QStringLiteral("Primary Xenon launcher profile."), true,
                                          QStringLiteral("profile-primary")));
  auto test = fixtureProfile(QStringLiteral("Test Profile"),
                             QStringLiteral("Fictional profile used to exercise profile switching and editing."),
                             false, QStringLiteral("profile-test"));
  test.insert(QStringLiteral("games"), 2);
  test.insert(QStringLiteral("modules"), 3);
  test.insert(QStringLiteral("saveSets"), 1);
  test.insert(QStringLiteral("gamePath"), QStringLiteral("TEST://Games"));
  test.insert(QStringLiteral("savePath"), QStringLiteral("TEST://Saves"));
  test.insert(QStringLiteral("screenshotPath"), QStringLiteral("TEST://Screenshots"));
  test.insert(QStringLiteral("isolatedSettings"), true);
  test.insert(QStringLiteral("runtimeOverrides"), runtimeDefaults());
  test.insert(QStringLiteral("lastUsedAt"), nowIso());
  test.insert(QStringLiteral("lastUsed"), QStringLiteral("Test fixture"));
  fixture_profiles_.append(test);
  fixture_active_index_ = 0;
  emit changed();
}

QString ProfilesFeature::fixtureCopyName(const QString& base_name) const {
  auto candidate = base_name + QStringLiteral(" Copy");
  if (nameAvailable(candidate)) return candidate;
  for (int suffix = 2; suffix < 10000; ++suffix) {
    candidate = base_name + QStringLiteral(" Copy %1").arg(suffix);
    if (nameAvailable(candidate)) return candidate;
  }
  return base_name + QStringLiteral(" Copy %1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces).left(8));
}

ServiceResult ProfilesFeature::normalizeProfileData(QVariantMap& data) const {
  static const QStringList regions{QStringLiteral("Auto (Global)"), QStringLiteral("NTSC-U"),
                                   QStringLiteral("PAL"), QStringLiteral("NTSC-J")};
  static const QStringList pages{QStringLiteral("Launcher default"), QStringLiteral("Home"),
                                 QStringLiteral("Library"), QStringLiteral("Modules"),
                                 QStringLiteral("Profiles"), QStringLiteral("Settings")};

  auto region = data.value(QStringLiteral("region"), QStringLiteral("Auto (Global)")).toString().trimmed();
  if (!containsValue(regions, region)) region = QStringLiteral("Auto (Global)");
  for (const auto& candidate : regions) {
    if (candidate.compare(region, Qt::CaseInsensitive) == 0) {
      region = candidate;
      break;
    }
  }
  data.insert(QStringLiteral("region"), region);

  auto startup = data.value(QStringLiteral("startupPage"), QStringLiteral("Launcher default")).toString().trimmed();
  if (!containsValue(pages, startup)) startup = QStringLiteral("Launcher default");
  for (const auto& candidate : pages) {
    if (candidate.compare(startup, Qt::CaseInsensitive) == 0) {
      startup = candidate;
      break;
    }
  }
  data.insert(QStringLiteral("startupPage"), startup);
  data.insert(QStringLiteral("offline"), data.contains(QStringLiteral("offline"))
      ? data.value(QStringLiteral("offline")).toBool()
      : settings_.boolValue(QStringLiteral("runtime/offline"), true));

  const auto isolated = data.value(QStringLiteral("isolatedSettings")).toBool();
  data.insert(QStringLiteral("isolatedSettings"), isolated);
  if (!isolated) {
    data.insert(QStringLiteral("runtimeOverrides"), QVariantMap{});
    return ServiceResult::success({}, {}, data);
  }

  auto overrides = data.value(QStringLiteral("runtimeOverrides")).toMap();
  if (overrides.isEmpty()) overrides = runtimeDefaults();
  const auto normalized = ProfileRuntimeCatalog::normalize(overrides, runtime_, test_mode_);
  if (!normalized.ok) return normalized;
  data.insert(QStringLiteral("runtimeOverrides"), normalized.data.toMap());
  return ServiceResult::success({}, {}, data);
}

ServiceResult ProfilesFeature::prepareAvatar(QVariantMap& data, int existing_index) {
  const auto profile_id = data.value(QStringLiteral("profileId"),
                                     existing_index >= 0 ? profile(existing_index).value(QStringLiteral("profileId"))
                                                         : QVariant{newProfileId()}).toString();
  data.insert(QStringLiteral("profileId"), profile_id);
  const auto remove = data.value(QStringLiteral("removeAvatarOnSave")).toBool();
  const auto pending = data.value(QStringLiteral("pendingAvatarSource")).toString().trimmed();
  data.remove(QStringLiteral("removeAvatarOnSave"));
  data.remove(QStringLiteral("pendingAvatarSource"));

  if (remove) {
    (void)removeAvatar(profile_id);
    data.insert(QStringLiteral("avatarPath"), QString{});
  } else if (!pending.isEmpty()) {
    const auto imported = importAvatar(profile_id, QUrl{pending});
    if (!imported.ok) return imported;
    data.insert(QStringLiteral("avatarPath"), imported.data.toString());
  }
  return ServiceResult::success();
}

ServiceResult ProfilesFeature::create(const QVariantMap& input) {
  auto data = input;
  const auto normalized = normalizeProfileData(data);
  if (!normalized.ok) return normalized;
  const auto avatar = prepareAvatar(data);
  if (!avatar.ok) return avatar;
  if (!test_mode_) return profiles_.create(data);

  auto name = limited(data.value(QStringLiteral("profileName")).toString(), profileNameLimit());
  if (name.isEmpty()) name = QStringLiteral("Profile %1").arg(fixture_profiles_.size() + 1);
  if (!nameAvailable(name)) {
    return ServiceResult::failure(QStringLiteral("Profile not created"),
                                  QStringLiteral("Choose a unique profile name and try again."));
  }
  auto item = fixtureProfile(name,
                             data.value(QStringLiteral("description"), QStringLiteral("Xenon launcher profile.")).toString(),
                             false, data.value(QStringLiteral("profileId"), newProfileId()).toString());
  for (const auto& key : {"avatarPath", "gamePath", "savePath", "screenshotPath", "region", "startupPage",
                          "offline", "isolatedSettings", "runtimeOverrides"}) {
    if (data.contains(QString::fromLatin1(key))) item.insert(QString::fromLatin1(key), data.value(QString::fromLatin1(key)));
  }
  fixture_profiles_.append(item);
  emit changed();
  return ServiceResult::success(QStringLiteral("Profile created"), QStringLiteral("%1 was created.").arg(name),
                                static_cast<int>(fixture_profiles_.size() - 1));
}

ServiceResult ProfilesFeature::update(int index, const QVariantMap& input) {
  auto data = input;
  const auto normalized = normalizeProfileData(data);
  if (!normalized.ok) return normalized;
  const auto avatar = prepareAvatar(data, index);
  if (!avatar.ok) return avatar;
  if (!test_mode_) return profiles_.update(index, data);
  if (index < 0 || index >= static_cast<int>(fixture_profiles_.size())) {
    return ServiceResult::failure(QStringLiteral("Profile not updated"), QStringLiteral("The selected profile no longer exists."));
  }
  const auto name = limited(data.value(QStringLiteral("profileName")).toString(), profileNameLimit());
  if (name.isEmpty() || !nameAvailable(name, index)) {
    return ServiceResult::failure(QStringLiteral("Profile not updated"), QStringLiteral("Choose a unique profile name and try again."));
  }
  auto item = fixture_profiles_.at(index).toMap();
  item.insert(QStringLiteral("profileName"), name);
  item.insert(QStringLiteral("description"), limited(data.value(QStringLiteral("description")).toString(), descriptionLimit()));
  for (const auto& key : {"avatarPath", "gamePath", "savePath", "screenshotPath", "region", "startupPage",
                          "offline", "isolatedSettings", "runtimeOverrides"}) {
    if (data.contains(QString::fromLatin1(key))) item.insert(QString::fromLatin1(key), data.value(QString::fromLatin1(key)));
  }
  fixture_profiles_[index] = item;
  emit changed();
  return ServiceResult::success(QStringLiteral("Profile updated"), QStringLiteral("%1 was updated.").arg(name));
}

ServiceResult ProfilesFeature::duplicate(int index) {
  if (!test_mode_) return profiles_.duplicate(index);
  if (index < 0 || index >= static_cast<int>(fixture_profiles_.size())) {
    return ServiceResult::failure(QStringLiteral("Profile duplication"), QStringLiteral("The selected profile no longer exists."));
  }
  auto item = fixture_profiles_.at(index).toMap();
  item.insert(QStringLiteral("profileId"), newProfileId());
  item.insert(QStringLiteral("profileName"), fixtureCopyName(item.value(QStringLiteral("profileName")).toString()));
  item.insert(QStringLiteral("active"), false);
  item.insert(QStringLiteral("createdAt"), nowIso());
  item.insert(QStringLiteral("lastUsedAt"), QString{});
  item.insert(QStringLiteral("lastUsed"), QStringLiteral("Not used yet"));
  fixture_profiles_.append(item);
  emit changed();
  return ServiceResult::success(QStringLiteral("Profile duplicated"),
                                QStringLiteral("%1 was created.").arg(item.value(QStringLiteral("profileName")).toString()),
                                static_cast<int>(fixture_profiles_.size() - 1));
}

ServiceResult ProfilesFeature::activate(int index) {
  if (!test_mode_) return profiles_.activate(index);
  if (index < 0 || index >= static_cast<int>(fixture_profiles_.size())) {
    return ServiceResult::failure(QStringLiteral("Profile activation"), QStringLiteral("The selected profile no longer exists."));
  }
  for (qsizetype i = 0; i < fixture_profiles_.size(); ++i) {
    auto item = fixture_profiles_.at(i).toMap();
    const auto active = static_cast<int>(i) == index;
    item.insert(QStringLiteral("active"), active);
    if (active) {
      item.insert(QStringLiteral("lastUsedAt"), nowIso());
      item.insert(QStringLiteral("lastUsed"), QStringLiteral("Current session"));
    }
    fixture_profiles_[i] = item;
  }
  fixture_active_index_ = index;
  emit changed();
  return ServiceResult::success(QStringLiteral("Profile activated"),
                                QStringLiteral("%1 is now active.").arg(profile(index).value(QStringLiteral("profileName")).toString()));
}

ServiceResult ProfilesFeature::remove(int index) {
  if (!test_mode_) return profiles_.remove(index);
  if (index < 0 || index >= static_cast<int>(fixture_profiles_.size()) || fixture_profiles_.size() <= 1) {
    return ServiceResult::failure(QStringLiteral("Profile not deleted"), QStringLiteral("The selected profile cannot be deleted."));
  }
  if (fixture_profiles_.at(index).toMap().value(QStringLiteral("active")).toBool()) {
    return ServiceResult::failure(QStringLiteral("Profile not deleted"), QStringLiteral("Activate another profile before deleting this one."));
  }
  const auto name = fixture_profiles_.at(index).toMap().value(QStringLiteral("profileName")).toString();
  fixture_profiles_.removeAt(index);
  if (fixture_active_index_ > index) --fixture_active_index_;
  emit changed();
  return ServiceResult::success(QStringLiteral("Profile deleted"), QStringLiteral("%1 was removed from the launcher.").arg(name));
}

ServiceResult ProfilesFeature::exportProfile(int index, const QUrl& destination) const {
  if (!test_mode_) return profiles_.exportProfile(index, destination);
  if (index < 0 || index >= static_cast<int>(fixture_profiles_.size())) {
    return ServiceResult::failure(QStringLiteral("Profile export"), QStringLiteral("The selected profile no longer exists."));
  }
  const auto normalized = normalizedDestination(destination);
  if (!normalized.isLocalFile()) {
    return ServiceResult::failure(QStringLiteral("Profile export"), QStringLiteral("Choose a local destination for the exported profile."));
  }
  auto exported = fixture_profiles_.at(index).toMap();
  for (const auto& key : {QStringLiteral("profileId"), QStringLiteral("active"),
                          QStringLiteral("avatarPath"), QStringLiteral("games"),
                          QStringLiteral("modules"), QStringLiteral("saveSets"),
                          QStringLiteral("gamePath"), QStringLiteral("savePath"),
                          QStringLiteral("screenshotPath"), QStringLiteral("createdAt"),
                          QStringLiteral("lastUsedAt"), QStringLiteral("lastUsed")}) {
    exported.remove(key);
  }
  QJsonObject root;
  root.insert(QStringLiteral("schema"), QStringLiteral("xenon.launcher.profile"));
  root.insert(QStringLiteral("version"), 1);
  root.insert(QStringLiteral("profile"), QJsonObject::fromVariantMap(exported));
  QSaveFile file{normalized.toLocalFile()};
  if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument{root}.toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
    return ServiceResult::failure(QStringLiteral("Profile export"), QStringLiteral("Xenon could not write the selected profile."));
  }
  return ServiceResult::success(QStringLiteral("Profile exported"), QStringLiteral("The profile was exported successfully."));
}

ServiceResult ProfilesFeature::importProfile(const QUrl& source) {
  if (!test_mode_) {
    const auto imported = profiles_.importProfile(source);
    if (!imported.ok) return imported;
    const auto index = imported.data.toInt();
    auto data = profiles_.profile(index);
    const auto normalized = normalizeProfileData(data);
    if (!normalized.ok) {
      (void)profiles_.remove(index);
      return normalized;
    }
    const auto saved = profiles_.update(index, data);
    if (!saved.ok) {
      (void)profiles_.remove(index);
      return saved;
    }
    return imported;
  }
  if (!source.isLocalFile()) {
    return ServiceResult::failure(QStringLiteral("Profile import"), QStringLiteral("Choose a local .xenonprofile file."));
  }
  QFile file{source.toLocalFile()};
  if (!file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > 1024 * 1024) {
    return ServiceResult::failure(QStringLiteral("Profile import"), QStringLiteral("The profile file could not be read."));
  }
  QJsonParseError error{};
  const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    return ServiceResult::failure(QStringLiteral("Profile import"), QStringLiteral("The profile file is not valid JSON."));
  }
  const auto root = document.object();
  if (root.value(QStringLiteral("schema")).toString() != QStringLiteral("xenon.launcher.profile") ||
      root.value(QStringLiteral("version")).toInt() != 1 ||
      !root.value(QStringLiteral("profile")).isObject()) {
    return ServiceResult::failure(QStringLiteral("Profile import"),
                                  QStringLiteral("The profile format or version is not supported by this launcher build."));
  }
  auto item = root.value(QStringLiteral("profile")).toObject().toVariantMap();
  auto name = limited(item.value(QStringLiteral("profileName"), QStringLiteral("Imported Profile")).toString(), profileNameLimit());
  if (!nameAvailable(name)) name = fixtureCopyName(name);
  item.insert(QStringLiteral("profileId"), newProfileId());
  item.insert(QStringLiteral("profileName"), name);
  item.insert(QStringLiteral("active"), false);
  item.insert(QStringLiteral("createdAt"), nowIso());
  item.insert(QStringLiteral("lastUsedAt"), QString{});
  item.insert(QStringLiteral("lastUsed"), QStringLiteral("Not used yet"));
  const auto normalized = normalizeProfileData(item);
  if (!normalized.ok) return normalized;
  fixture_profiles_.append(item);
  emit changed();
  return ServiceResult::success(QStringLiteral("Profile imported"), QStringLiteral("%1 was imported.").arg(name),
                                static_cast<int>(fixture_profiles_.size() - 1));
}

ServiceResult ProfilesFeature::persistToCurrentStorage() {
  if (test_mode_) return ServiceResult::success();
  const auto json = QJsonDocument{QJsonArray::fromVariantList(profiles_.profiles())}
                        .toJson(QJsonDocument::Indented);
  return profiles_.saveState(QString::fromUtf8(json));
}

ServiceResult ProfilesFeature::importAvatar(const QString& profile_id, const QUrl& source_url) {
  if (!test_mode_) return profiles_.importAvatar(profile_id, source_url);
  const auto local = source_url.isLocalFile() ? source_url.toLocalFile() : source_url.toString();
  const QFileInfo info{local};
  if (local.trimmed().isEmpty() || !info.exists() || !info.isFile()) {
    return ServiceResult::failure(QStringLiteral("Profile image"),
                                  QStringLiteral("The selected image could not be opened."));
  }
  const auto extension = info.suffix().toLower();
  if (extension != QStringLiteral("png") && extension != QStringLiteral("jpg") &&
      extension != QStringLiteral("jpeg") && extension != QStringLiteral("webp")) {
    return ServiceResult::failure(QStringLiteral("Profile image"),
                                  QStringLiteral("Choose a PNG, JPEG, or WebP image."));
  }
  return ServiceResult::success(QStringLiteral("Profile image selected"), {},
                                QUrl::fromLocalFile(info.absoluteFilePath()).toString());
}

bool ProfilesFeature::removeAvatar(const QString& profile_id) {
  return test_mode_ ? !profile_id.trimmed().isEmpty() : profiles_.removeAvatar(profile_id);
}

ServiceResult ProfilesFeature::removeAvatarForProfile(int index) {
  auto item = profile(index);
  if (item.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Profile image"), QStringLiteral("The selected profile no longer exists."));
  }
  if (item.value(QStringLiteral("avatarPath")).toString().trimmed().isEmpty()) {
    return ServiceResult::success(QStringLiteral("Profile image"), QStringLiteral("This profile does not have a custom image."));
  }
  const auto name = item.value(QStringLiteral("profileName")).toString();
  item.insert(QStringLiteral("removeAvatarOnSave"), true);
  item.insert(QStringLiteral("pendingAvatarSource"), QString{});
  const auto result = update(index, item);
  if (!result.ok) return result;
  return ServiceResult::success(QStringLiteral("Profile image removed"),
                                QStringLiteral("%1 now uses the generated profile mark.").arg(name));
}

}  // namespace xenon::launcher::frontend_backend
