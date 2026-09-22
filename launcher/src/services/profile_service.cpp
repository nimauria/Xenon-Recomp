#include "profile_service.hpp"

#include "path_service.hpp"
#include "settings_service.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

namespace xenon::launcher {
namespace {
constexpr int kProfileNameLimit = 48;
constexpr int kDescriptionLimit = 180;
// The crop editor's own upper bound on how far a user may zoom in past the
// minimum cover scale; kept in sync with AvatarCropEditor.qml's slider range.
constexpr double kMaxAvatarZoom = 3.0;

QString limited(QString value, int limit) {
  value = value.trimmed();
  if (value.size() > limit) value.truncate(limit);
  return value;
}

QString nowIso() {
  return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

bool safeProfileId(const QString& value) {
  static const QRegularExpression pattern{QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")};
  const auto trimmed = value.trimmed();
  return pattern.match(trimmed).hasMatch() && trimmed != QStringLiteral(".") &&
         trimmed != QStringLiteral("..") && !trimmed.contains(QStringLiteral(".."));
}
}

ProfileService::ProfileService(SettingsService& settings, PathService& paths, QObject* parent)
    : QObject(parent), settings_(settings), paths_(paths) {}

void ProfileService::initialize(const QString& primary_name) {
  const auto raw = loadState();
  if (!raw.trimmed().isEmpty()) {
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(raw.toUtf8(), &error);
    if (error.error == QJsonParseError::NoError && document.isArray() && !document.array().isEmpty()) {
      normalizeLoadedProfiles(document.array().toVariantList(), primary_name);
      return;
    }
  }

  profiles_.clear();
  profiles_.append(defaultProfile(primary_name.trimmed().isEmpty() ? QStringLiteral("Nimauria") : primary_name,
                                  true, QStringLiteral("profile-primary")));
  active_index_ = 0;
  (void)persistProfiles();
}

QVariantList ProfileService::profiles() const { return profiles_; }

QVariantMap ProfileService::profile(int index) const {
  if (profiles_.isEmpty()) return {};
  const auto safe = qBound(0, index, static_cast<int>(profiles_.size()) - 1);
  return profiles_.at(safe).toMap();
}

QVariantMap ProfileService::activeProfile() const { return profile(active_index_); }
int ProfileService::activeIndex() const noexcept { return active_index_; }

bool ProfileService::nameAvailable(const QString& name, int exclude_index) const {
  const auto needle = limited(name, kProfileNameLimit).toLower();
  if (needle.isEmpty()) return false;
  for (qsizetype i = 0; i < profiles_.size(); ++i) {
    if (i == exclude_index) continue;
    if (profiles_.at(i).toMap().value(QStringLiteral("profileName")).toString().trimmed().toLower() == needle)
      return false;
  }
  return true;
}

QString ProfileService::newProfileId() const {
  return QStringLiteral("profile-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

ServiceResult ProfileService::create(const QVariantMap& data) {
  auto name = limited(data.value(QStringLiteral("profileName")).toString(), kProfileNameLimit);
  if (name.isEmpty()) name = QStringLiteral("Profile %1").arg(profiles_.size() + 1);
  if (!nameAvailable(name)) {
    return ServiceResult::failure(QStringLiteral("Profile not created"),
                                  QStringLiteral("Choose a unique profile name and try again."));
  }

  const auto requested_id = data.value(QStringLiteral("profileId")).toString().trimmed();
  auto item = defaultProfile(name, false, safeProfileId(requested_id) ? requested_id : newProfileId());
  item.insert(QStringLiteral("description"), limited(data.value(QStringLiteral("description"), QStringLiteral("Xenon launcher profile.")).toString(), kDescriptionLimit));
  item.insert(QStringLiteral("avatarPath"), data.value(QStringLiteral("avatarPath")).toString());
  // The crop editor's position/zoom is metadata over the untouched original
  // image (see importAvatar()) - never baked into the file - so it is always
  // saved alongside avatarPath rather than destructively re-encoding it.
  item.insert(QStringLiteral("avatarFocalX"), qBound(0.0, data.value(QStringLiteral("avatarFocalX"), 0.5).toDouble(), 1.0));
  item.insert(QStringLiteral("avatarFocalY"), qBound(0.0, data.value(QStringLiteral("avatarFocalY"), 0.5).toDouble(), 1.0));
  item.insert(QStringLiteral("avatarZoom"), qBound(1.0, data.value(QStringLiteral("avatarZoom"), 1.0).toDouble(), kMaxAvatarZoom));
  item.insert(QStringLiteral("gamePath"), data.value(QStringLiteral("gamePath")).toString());
  item.insert(QStringLiteral("savePath"), data.value(QStringLiteral("savePath")).toString());
  item.insert(QStringLiteral("screenshotPath"), data.value(QStringLiteral("screenshotPath")).toString());
  item.insert(QStringLiteral("region"), data.value(QStringLiteral("region"), QStringLiteral("Auto (Global)")));
  item.insert(QStringLiteral("startupPage"), data.value(QStringLiteral("startupPage"), QStringLiteral("Launcher default")));
  item.insert(QStringLiteral("offline"), data.contains(QStringLiteral("offline")) ? data.value(QStringLiteral("offline")).toBool() : true);
  item.insert(QStringLiteral("isolatedSettings"), data.value(QStringLiteral("isolatedSettings")).toBool());
  item.insert(QStringLiteral("runtimeOverrides"), data.value(QStringLiteral("runtimeOverrides")).toMap());
  profiles_.append(item);
  if (!persistProfiles()) {
    profiles_.removeLast();
    return ServiceResult::failure(QStringLiteral("Profile not created"),
                                  QStringLiteral("Xenon could not save the new profile."));
  }
  emit changed();
  return ServiceResult::success(QStringLiteral("Profile created"),
                                QStringLiteral("%1 was created.").arg(name),
                                static_cast<int>(profiles_.size() - 1));
}

ServiceResult ProfileService::update(int index, const QVariantMap& data) {
  if (index < 0 || index >= static_cast<int>(profiles_.size())) {
    return ServiceResult::failure(QStringLiteral("Profile not updated"),
                                  QStringLiteral("The selected profile no longer exists."));
  }
  auto item = profiles_.at(index).toMap();
  const auto name = limited(data.value(QStringLiteral("profileName")).toString(), kProfileNameLimit);
  if (name.isEmpty() || !nameAvailable(name, index)) {
    return ServiceResult::failure(QStringLiteral("Profile not updated"),
                                  QStringLiteral("Choose a unique profile name and try again."));
  }
  item.insert(QStringLiteral("profileName"), name);
  item.insert(QStringLiteral("description"), limited(data.value(QStringLiteral("description")).toString(), kDescriptionLimit));
  if (data.contains(QStringLiteral("avatarPath"))) item.insert(QStringLiteral("avatarPath"), data.value(QStringLiteral("avatarPath")).toString());
  if (data.contains(QStringLiteral("avatarFocalX")))
    item.insert(QStringLiteral("avatarFocalX"), qBound(0.0, data.value(QStringLiteral("avatarFocalX")).toDouble(), 1.0));
  if (data.contains(QStringLiteral("avatarFocalY")))
    item.insert(QStringLiteral("avatarFocalY"), qBound(0.0, data.value(QStringLiteral("avatarFocalY")).toDouble(), 1.0));
  if (data.contains(QStringLiteral("avatarZoom")))
    item.insert(QStringLiteral("avatarZoom"), qBound(1.0, data.value(QStringLiteral("avatarZoom")).toDouble(), kMaxAvatarZoom));
  item.insert(QStringLiteral("gamePath"), data.value(QStringLiteral("gamePath")).toString());
  item.insert(QStringLiteral("savePath"), data.value(QStringLiteral("savePath")).toString());
  item.insert(QStringLiteral("screenshotPath"), data.value(QStringLiteral("screenshotPath")).toString());
  item.insert(QStringLiteral("region"), data.value(QStringLiteral("region"), QStringLiteral("Auto (Global)")));
  item.insert(QStringLiteral("startupPage"), data.value(QStringLiteral("startupPage"), QStringLiteral("Launcher default")));
  item.insert(QStringLiteral("offline"), data.value(QStringLiteral("offline")).toBool());
  item.insert(QStringLiteral("isolatedSettings"), data.value(QStringLiteral("isolatedSettings")).toBool());
  item.insert(QStringLiteral("runtimeOverrides"), data.value(QStringLiteral("runtimeOverrides")).toMap());

  const auto previous = profiles_.at(index);
  profiles_[index] = item;
  if (!persistProfiles()) {
    profiles_[index] = previous;
    return ServiceResult::failure(QStringLiteral("Profile not updated"),
                                  QStringLiteral("Xenon could not save the profile changes."));
  }
  emit changed();
  return ServiceResult::success(QStringLiteral("Profile updated"),
                                QStringLiteral("%1 was updated.").arg(name));
}

ServiceResult ProfileService::duplicate(int index) {
  if (index < 0 || index >= static_cast<int>(profiles_.size())) {
    return ServiceResult::failure(QStringLiteral("Profile duplication"),
                                  QStringLiteral("The selected profile no longer exists."));
  }
  auto item = profiles_.at(index).toMap();
  const auto new_id = newProfileId();
  item.insert(QStringLiteral("profileId"), new_id);
  item.insert(QStringLiteral("profileName"), uniqueCopyName(item.value(QStringLiteral("profileName")).toString()));
  item.insert(QStringLiteral("active"), false);
  item.insert(QStringLiteral("createdAt"), nowIso());
  item.insert(QStringLiteral("lastUsedAt"), QString{});
  item.insert(QStringLiteral("lastUsed"), QStringLiteral("Not used yet"));

  const auto avatar = item.value(QStringLiteral("avatarPath")).toString();
  if (!avatar.isEmpty()) {
    const auto copied = importAvatar(new_id, QUrl{avatar});
    item.insert(QStringLiteral("avatarPath"), copied.ok ? copied.data.toString() : QString{});
  }

  profiles_.append(item);
  if (!persistProfiles()) {
    profiles_.removeLast();
    (void)removeAvatar(new_id);
    return ServiceResult::failure(QStringLiteral("Profile duplication"),
                                  QStringLiteral("Xenon could not save the duplicated profile."));
  }
  emit changed();
  return ServiceResult::success(QStringLiteral("Profile duplicated"),
                                QStringLiteral("%1 was created.").arg(item.value(QStringLiteral("profileName")).toString()),
                                static_cast<int>(profiles_.size() - 1));
}

ServiceResult ProfileService::activate(int index) {
  if (index < 0 || index >= static_cast<int>(profiles_.size())) {
    return ServiceResult::failure(QStringLiteral("Profile activation"),
                                  QStringLiteral("The selected profile no longer exists."));
  }
  const auto previous = profiles_;
  const auto previous_active = active_index_;
  for (qsizetype i = 0; i < profiles_.size(); ++i) {
    auto item = profiles_.at(i).toMap();
    const auto active = static_cast<int>(i) == index;
    item.insert(QStringLiteral("active"), active);
    if (active) {
      item.insert(QStringLiteral("lastUsedAt"), nowIso());
      item.insert(QStringLiteral("lastUsed"), QStringLiteral("Current session"));
    }
    profiles_[i] = item;
  }
  active_index_ = index;
  if (!persistProfiles()) {
    profiles_ = previous;
    active_index_ = previous_active;
    return ServiceResult::failure(QStringLiteral("Profile activation"),
                                  QStringLiteral("Xenon could not save the active profile."));
  }
  emit changed();
  const auto name = profile(index).value(QStringLiteral("profileName")).toString();
  return ServiceResult::success(QStringLiteral("Profile activated"),
                                QStringLiteral("%1 is now the active profile.").arg(name));
}

ServiceResult ProfileService::remove(int index) {
  if (index < 0 || index >= static_cast<int>(profiles_.size()) || profiles_.size() <= 1) {
    return ServiceResult::failure(QStringLiteral("Profile not deleted"),
                                  QStringLiteral("At least one launcher profile must remain."));
  }
  const auto item = profiles_.at(index).toMap();
  if (item.value(QStringLiteral("active")).toBool()) {
    return ServiceResult::failure(QStringLiteral("Profile protected"),
                                  QStringLiteral("Activate another profile before deleting this one."));
  }
  const auto previous = profiles_;
  const auto previous_active = active_index_;
  profiles_.removeAt(index);
  if (active_index_ > index) --active_index_;
  if (!persistProfiles()) {
    profiles_ = previous;
    active_index_ = previous_active;
    return ServiceResult::failure(QStringLiteral("Profile not deleted"),
                                  QStringLiteral("Xenon could not save the profile list."));
  }
  (void)removeAvatar(item.value(QStringLiteral("profileId")).toString());
  emit changed();
  return ServiceResult::success(QStringLiteral("Profile deleted"),
                                QStringLiteral("%1 was removed from the launcher.")
                                    .arg(item.value(QStringLiteral("profileName")).toString()));
}

ServiceResult ProfileService::exportProfile(int index, const QUrl& destination) const {
  if (index < 0 || index >= static_cast<int>(profiles_.size())) {
    return ServiceResult::failure(QStringLiteral("Profile export"),
                                  QStringLiteral("The selected profile no longer exists."));
  }
  if (!destination.isLocalFile()) {
    return ServiceResult::failure(QStringLiteral("Profile export"),
                                  QStringLiteral("Choose a local destination for the exported profile."));
  }

  auto path = destination.toLocalFile();
  if (!path.endsWith(QStringLiteral(".xenonprofile"), Qt::CaseInsensitive))
    path += QStringLiteral(".xenonprofile");

  auto exported = profiles_.at(index).toMap();
  for (const auto& key : {QStringLiteral("profileId"), QStringLiteral("active"),
                          QStringLiteral("avatarPath"), QStringLiteral("games"),
                          QStringLiteral("modules"), QStringLiteral("saveSets"),
                          QStringLiteral("gamePath"), QStringLiteral("savePath"),
                          QStringLiteral("screenshotPath"),
                          QStringLiteral("createdAt"), QStringLiteral("lastUsedAt"),
                          QStringLiteral("lastUsed")}) {
    exported.remove(key);
  }

  QJsonObject root;
  root.insert(QStringLiteral("schema"), QStringLiteral("xenon.launcher.profile"));
  root.insert(QStringLiteral("version"), 1);
  root.insert(QStringLiteral("profile"), QJsonObject::fromVariantMap(exported));

  QSaveFile file{path};
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    return ServiceResult::failure(QStringLiteral("Profile export"),
                                  QStringLiteral("Xenon could not open the selected export destination."));
  }
  file.write(QJsonDocument{root}.toJson(QJsonDocument::Indented));
  if (!file.commit()) {
    return ServiceResult::failure(QStringLiteral("Profile export"),
                                  QStringLiteral("Xenon could not commit the exported profile safely."));
  }
  return ServiceResult::success(QStringLiteral("Profile exported"),
                                QStringLiteral("%1 was exported without machine-local paths, avatar, or history data.")
                                    .arg(profiles_.at(index).toMap().value(QStringLiteral("profileName")).toString()),
                                QUrl::fromLocalFile(path));
}

ServiceResult ProfileService::importProfile(const QUrl& source) {
  if (!source.isLocalFile()) {
    return ServiceResult::failure(QStringLiteral("Profile import"),
                                  QStringLiteral("Choose a local Xenon profile file."));
  }
  QFile file{source.toLocalFile()};
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text) || file.size() > 1024 * 1024) {
    return ServiceResult::failure(QStringLiteral("Profile import"),
                                  QStringLiteral("The selected profile file could not be read or is unexpectedly large."));
  }

  QJsonParseError error{};
  const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) {
    return ServiceResult::failure(QStringLiteral("Profile import"),
                                  QStringLiteral("The selected file is not valid Xenon profile JSON."));
  }

  const auto root = document.object();
  if (root.value(QStringLiteral("schema")).toString() != QStringLiteral("xenon.launcher.profile") ||
      root.value(QStringLiteral("version")).toInt() != 1 ||
      !root.value(QStringLiteral("profile")).isObject()) {
    return ServiceResult::failure(QStringLiteral("Profile import"),
                                  QStringLiteral("The profile format or version is not supported by this launcher build."));
  }

  auto data = root.value(QStringLiteral("profile")).toObject().toVariantMap();
  auto name = limited(data.value(QStringLiteral("profileName"), QStringLiteral("Imported Profile")).toString(),
                      kProfileNameLimit);
  if (name.isEmpty()) name = QStringLiteral("Imported Profile");
  if (!nameAvailable(name)) name = uniqueCopyName(name);
  data.insert(QStringLiteral("profileName"), name);
  data.insert(QStringLiteral("profileId"), newProfileId());
  data.insert(QStringLiteral("avatarPath"), QString{});

  const auto result = create(data);
  if (!result.ok) return result;
  return ServiceResult::success(QStringLiteral("Profile imported"),
                                QStringLiteral("%1 was imported as a new local profile.").arg(name),
                                result.data);
}

QString ProfileService::profileRoot() const { return paths_.configuredPath(QStringLiteral("profiles")); }

QString ProfileService::loadState() const {
  const auto file_path = QDir{profileRoot()}.filePath(QStringLiteral("profiles.json"));
  QFile file{file_path};
  if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    const auto data = QString::fromUtf8(file.readAll()).trimmed();
    if (!data.isEmpty()) return data;
  }
  return settings_.stringValue(QStringLiteral("frontend/profiles/state"));
}

ServiceResult ProfileService::saveState(const QString& json) {
  settings_.setValue(QStringLiteral("frontend/profiles/state"), json);
  const auto root = profileRoot();
  if (!paths_.ensureDirectory(root)) {
    return ServiceResult::failure(QStringLiteral("Profile storage error"),
                                  QStringLiteral("Xenon could not create the configured profile folder."));
  }
  QSaveFile file{QDir{root}.filePath(QStringLiteral("profiles.json"))};
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
    return ServiceResult::failure(QStringLiteral("Profile storage error"),
                                  QStringLiteral("Xenon could not write profiles.json in the configured profile folder."));
  }
  file.write(json.toUtf8());
  if (!file.commit()) {
    return ServiceResult::failure(QStringLiteral("Profile storage error"),
                                  QStringLiteral("Xenon could not commit profiles.json safely."));
  }
  return ServiceResult::success();
}

ServiceResult ProfileService::importAvatar(const QString& profile_id, const QUrl& source_url) {
  const auto source = source_url.isLocalFile() ? source_url.toLocalFile() : source_url.toString();
  const QFileInfo source_info{source};
  if (!source_info.exists() || !source_info.isFile()) {
    return ServiceResult::failure(QStringLiteral("Profile image"),
                                  QStringLiteral("The selected image could not be opened."));
  }
  auto extension = source_info.suffix().toLower();
  if (extension != QStringLiteral("png") && extension != QStringLiteral("jpg") &&
      extension != QStringLiteral("jpeg") && extension != QStringLiteral("webp")) {
    return ServiceResult::failure(QStringLiteral("Profile image"),
                                  QStringLiteral("Choose a PNG, JPEG, or WebP image."));
  }
  if (extension == QStringLiteral("jpeg")) extension = QStringLiteral("jpg");

  const auto candidate_id = profile_id.trimmed().isEmpty() ? QStringLiteral("profile-temp") : profile_id.trimmed();
  if (!safeProfileId(candidate_id)) {
    return ServiceResult::failure(QStringLiteral("Profile image"),
                                  QStringLiteral("The profile identifier is not valid for local storage."));
  }
  const auto safe_id = candidate_id;
  const auto destination_dir = QDir{profileRoot()}.filePath(safe_id);
  if (!paths_.ensureDirectory(destination_dir)) {
    return ServiceResult::failure(QStringLiteral("Profile image"),
                                  QStringLiteral("Xenon could not create the profile image folder."));
  }
  (void)removeAvatar(safe_id);
  const auto destination = QDir{destination_dir}.filePath(QStringLiteral("avatar.%1").arg(extension));
  if (!QFile::copy(source, destination)) {
    return ServiceResult::failure(QStringLiteral("Profile image"),
                                  QStringLiteral("Xenon could not copy the selected profile image."));
  }
  return ServiceResult::success(QString{}, QString{}, QUrl::fromLocalFile(destination).toString());
}

bool ProfileService::removeAvatar(const QString& profile_id) const {
  if (!safeProfileId(profile_id)) return false;
  QDir dir{QDir{profileRoot()}.filePath(profile_id)};
  if (!dir.exists()) return true;
  bool ok = true;
  const auto avatars = dir.entryList({QStringLiteral("avatar.*")}, QDir::Files);
  for (const auto& avatar : avatars) if (!dir.remove(avatar)) ok = false;
  return ok;
}

QVariantMap ProfileService::defaultProfile(const QString& name, bool active,
                                           const QString& profile_id) const {
  QVariantMap item;
  item.insert(QStringLiteral("profileId"), profile_id.isEmpty() ? newProfileId() : profile_id);
  item.insert(QStringLiteral("profileName"), limited(name, kProfileNameLimit));
  item.insert(QStringLiteral("description"), QStringLiteral("Xenon launcher profile."));
  item.insert(QStringLiteral("active"), active);
  item.insert(QStringLiteral("games"), 0);
  item.insert(QStringLiteral("avatarPath"), QString{});
  item.insert(QStringLiteral("avatarFocalX"), 0.5);
  item.insert(QStringLiteral("avatarFocalY"), 0.5);
  item.insert(QStringLiteral("avatarZoom"), 1.0);
  item.insert(QStringLiteral("modules"), 0);
  item.insert(QStringLiteral("saveSets"), 0);
  item.insert(QStringLiteral("gamePath"), QString{});
  item.insert(QStringLiteral("savePath"), QString{});
  item.insert(QStringLiteral("screenshotPath"), QString{});
  item.insert(QStringLiteral("region"), QStringLiteral("Auto (Global)"));
  item.insert(QStringLiteral("startupPage"), QStringLiteral("Launcher default"));
  item.insert(QStringLiteral("offline"), settings_.boolValue(QStringLiteral("runtime/offline"), true));
  item.insert(QStringLiteral("isolatedSettings"), false);
  item.insert(QStringLiteral("runtimeOverrides"), QVariantMap{});
  item.insert(QStringLiteral("createdAt"), nowIso());
  item.insert(QStringLiteral("lastUsedAt"), active ? nowIso() : QString{});
  item.insert(QStringLiteral("lastUsed"), active ? QStringLiteral("Current session") : QStringLiteral("Not used yet"));
  return item;
}

QString ProfileService::uniqueCopyName(const QString& base_name) const {
  const auto root = base_name + QStringLiteral(" Copy");
  if (nameAvailable(root)) return root;
  for (int suffix = 2;; ++suffix) {
    const auto candidate = QStringLiteral("%1 %2").arg(root).arg(suffix);
    if (nameAvailable(candidate)) return candidate;
  }
}

bool ProfileService::persistProfiles() {
  QJsonArray array;
  for (const auto& value : profiles_) array.append(QJsonObject::fromVariantMap(value.toMap()));
  const auto result = saveState(QString::fromUtf8(QJsonDocument{array}.toJson(QJsonDocument::Compact)));
  return result.ok;
}

void ProfileService::normalizeLoadedProfiles(const QVariantList& parsed, const QString& primary_name) {
  profiles_.clear();
  bool found_active = false;
  active_index_ = 0;
  for (qsizetype i = 0; i < parsed.size(); ++i) {
    const auto source = parsed.at(i).toMap();
    auto item = defaultProfile(
        source.value(QStringLiteral("profileName"), i == 0 ? primary_name : QStringLiteral("Profile %1").arg(i + 1)).toString(),
        false,
        safeProfileId(source.value(QStringLiteral("profileId")).toString())
            ? source.value(QStringLiteral("profileId")).toString()
            : QStringLiteral("profile-restored-%1").arg(i));
    for (const auto& key : {QStringLiteral("description"), QStringLiteral("avatarPath"),
                            QStringLiteral("avatarFocalX"), QStringLiteral("avatarFocalY"),
                            QStringLiteral("avatarZoom"),
                            QStringLiteral("gamePath"), QStringLiteral("savePath"),
                            QStringLiteral("screenshotPath"), QStringLiteral("region"),
                            QStringLiteral("startupPage"), QStringLiteral("createdAt"),
                            QStringLiteral("lastUsedAt"), QStringLiteral("lastUsed")}) {
      if (source.contains(key)) item.insert(key, source.value(key));
    }
    for (const auto& key : {QStringLiteral("games"), QStringLiteral("modules"), QStringLiteral("saveSets")})
      if (source.contains(key)) item.insert(key, source.value(key).toInt());
    item.insert(QStringLiteral("offline"), source.contains(QStringLiteral("offline"))
        ? source.value(QStringLiteral("offline")).toBool()
        : settings_.boolValue(QStringLiteral("runtime/offline"), true));
    item.insert(QStringLiteral("isolatedSettings"), source.value(QStringLiteral("isolatedSettings")).toBool());
    item.insert(QStringLiteral("runtimeOverrides"), source.value(QStringLiteral("runtimeOverrides")).toMap());

    const auto active = source.value(QStringLiteral("active")).toBool() && !found_active;
    item.insert(QStringLiteral("active"), active);
    if (active) {
      found_active = true;
      active_index_ = static_cast<int>(i);
    }
    profiles_.append(item);
  }
  if (profiles_.isEmpty()) profiles_.append(defaultProfile(primary_name, true, QStringLiteral("profile-primary")));
  if (!found_active) {
    auto first = profiles_.first().toMap();
    first.insert(QStringLiteral("active"), true);
    profiles_[0] = first;
    active_index_ = 0;
  }
  (void)persistProfiles();
}

}  // namespace xenon::launcher
