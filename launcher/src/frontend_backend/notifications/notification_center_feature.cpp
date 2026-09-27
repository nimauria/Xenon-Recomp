#include "notification_center_feature.hpp"

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

#include <algorithm>

namespace xenon::launcher::frontend_backend {
namespace {
constexpr int kMaxNotifications = 120;
constexpr qint64 kDuplicateWindowMs = 30000;

QJsonObject toJson(const QVariantMap& record) {
  return QJsonObject::fromVariantMap(record);
}
}

NotificationCenterFeature::NotificationCenterFeature(PathService& paths, QObject* parent)
    : QObject(parent), paths_(paths) {
  (void)load();
}

QVariantList NotificationCenterFeature::entries() const {
  QVariantList result;
  result.reserve(records_.size());
  for (const auto& value : records_) result.append(publicRecord(value.toMap()));
  return result;
}

QVariantMap NotificationCenterFeature::entry(const QString& notification_id) const {
  const auto id = notification_id.trimmed();
  for (const auto& value : records_) {
    const auto record = value.toMap();
    if (record.value(QStringLiteral("id")).toString() == id) return publicRecord(record);
  }
  return {};
}

int NotificationCenterFeature::unreadCount() const noexcept { return unread_count_; }

QString NotificationCenterFeature::storagePath() const {
  return QDir{paths_.appDataPath()}.filePath(QStringLiteral("Notifications/history.json"));
}

QString NotificationCenterFeature::normalizeSeverity(const QString& severity) {
  const auto normalized = severity.trimmed().toLower();
  if (normalized == QStringLiteral("success") || normalized == QStringLiteral("warning") ||
      normalized == QStringLiteral("error")) {
    return normalized;
  }
  return QStringLiteral("info");
}

QVariantMap NotificationCenterFeature::publicRecord(const QVariantMap& record) {
  auto result = record;
  // Internal de-duplication metadata is intentionally not part of the QML contract.
  result.remove(QStringLiteral("dedupeKey"));
  return result;
}

ServiceResult NotificationCenterFeature::add(const QString& title, const QString& message,
                                             const QString& severity, const QString& source,
                                             const QString& command_id, const QString& target_id,
                                             const QString& section_id, const QString& action_label) {
  const auto clean_title = title.trimmed().isEmpty() ? QStringLiteral("Xenon Launcher") : title.trimmed();
  const auto clean_message = message.trimmed();
  if (clean_message.isEmpty()) return ServiceResult::success();

  const auto now = QDateTime::currentDateTimeUtc();
  const auto normalized_severity = normalizeSeverity(severity);
  const auto clean_source = source.trimmed().isEmpty() ? QStringLiteral("launcher") : source.trimmed().toLower();
  const auto dedupe_key = QStringLiteral("%1\n%2\n%3\n%4\n%5")
                              .arg(clean_source, normalized_severity, clean_title, clean_message,
                                   command_id.trimmed());

  if (!records_.isEmpty()) {
    auto newest = records_.first().toMap();
    const auto newest_time = QDateTime::fromString(newest.value(QStringLiteral("timestamp")).toString(), Qt::ISODateWithMs);
    if (newest.value(QStringLiteral("dedupeKey")).toString() == dedupe_key && newest_time.isValid() &&
        newest_time.msecsTo(now) >= 0 && newest_time.msecsTo(now) <= kDuplicateWindowMs) {
      const auto was_read = newest.value(QStringLiteral("read")).toBool();
      newest.insert(QStringLiteral("timestamp"), now.toString(Qt::ISODateWithMs));
      newest.insert(QStringLiteral("read"), false);
      newest.insert(QStringLiteral("repeatCount"), newest.value(QStringLiteral("repeatCount"), 1).toInt() + 1);
      if (!target_id.trimmed().isEmpty()) newest.insert(QStringLiteral("targetId"), target_id.trimmed());
      if (!section_id.trimmed().isEmpty()) newest.insert(QStringLiteral("sectionId"), section_id.trimmed());
      if (!action_label.trimmed().isEmpty()) newest.insert(QStringLiteral("actionLabel"), action_label.trimmed());
      records_[0] = newest;
      if (was_read) ++unread_count_;
      (void)save();
      emit changed();
      return ServiceResult::success();
    }
  }

  QVariantMap record{
      {QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
      {QStringLiteral("title"), clean_title},
      {QStringLiteral("message"), clean_message},
      {QStringLiteral("severity"), normalized_severity},
      {QStringLiteral("source"), clean_source},
      {QStringLiteral("timestamp"), now.toString(Qt::ISODateWithMs)},
      {QStringLiteral("read"), false},
      {QStringLiteral("repeatCount"), 1},
      {QStringLiteral("commandId"), command_id.trimmed()},
      {QStringLiteral("targetId"), target_id.trimmed()},
      {QStringLiteral("sectionId"), section_id.trimmed()},
      {QStringLiteral("actionLabel"), action_label.trimmed()},
      {QStringLiteral("dedupeKey"), dedupe_key},
  };
  records_.prepend(record);
  ++unread_count_;
  trim();
  (void)save();
  emit changed();
  return ServiceResult::success({}, {}, record.value(QStringLiteral("id")));
}

ServiceResult NotificationCenterFeature::markRead(const QString& notification_id, bool read) {
  const auto id = notification_id.trimmed();
  for (int i = 0; i < records_.size(); ++i) {
    auto record = records_.at(i).toMap();
    if (record.value(QStringLiteral("id")).toString() != id) continue;
    const auto current = record.value(QStringLiteral("read")).toBool();
    if (current == read) return ServiceResult::success();
    record.insert(QStringLiteral("read"), read);
    records_[i] = record;
    unread_count_ += read ? -1 : 1;
    unread_count_ = std::max(0, unread_count_);
    (void)save();
    emit changed();
    return ServiceResult::success();
  }
  return ServiceResult::failure(QStringLiteral("Notifications"), QStringLiteral("That notification no longer exists."));
}

ServiceResult NotificationCenterFeature::markAllRead() {
  if (unread_count_ == 0) return ServiceResult::success();
  for (int i = 0; i < records_.size(); ++i) {
    auto record = records_.at(i).toMap();
    record.insert(QStringLiteral("read"), true);
    records_[i] = record;
  }
  unread_count_ = 0;
  (void)save();
  emit changed();
  return ServiceResult::success();
}

ServiceResult NotificationCenterFeature::dismiss(const QString& notification_id) {
  const auto id = notification_id.trimmed();
  for (int i = 0; i < records_.size(); ++i) {
    const auto record = records_.at(i).toMap();
    if (record.value(QStringLiteral("id")).toString() != id) continue;
    if (!record.value(QStringLiteral("read")).toBool()) unread_count_ = std::max(0, unread_count_ - 1);
    records_.removeAt(i);
    (void)save();
    emit changed();
    return ServiceResult::success();
  }
  return ServiceResult::failure(QStringLiteral("Notifications"), QStringLiteral("That notification no longer exists."));
}

ServiceResult NotificationCenterFeature::clear() {
  if (records_.isEmpty()) return ServiceResult::success();
  records_.clear();
  unread_count_ = 0;
  (void)save();
  emit changed();
  return ServiceResult::success();
}

void NotificationCenterFeature::trim() {
  while (records_.size() > kMaxNotifications) {
    const auto record = records_.takeLast().toMap();
    if (!record.value(QStringLiteral("read")).toBool()) unread_count_ = std::max(0, unread_count_ - 1);
  }
}

bool NotificationCenterFeature::load() {
  records_.clear();
  unread_count_ = 0;
  QFile file{storagePath()};
  if (!file.exists()) return true;
  if (!file.open(QIODevice::ReadOnly)) return false;
  const auto document = QJsonDocument::fromJson(file.readAll());
  if (!document.isObject()) return false;
  const auto object = document.object();
  if (object.value(QStringLiteral("version")).toInt() != 1 || !object.value(QStringLiteral("notifications")).isArray()) return false;

  for (const auto& value : object.value(QStringLiteral("notifications")).toArray()) {
    if (!value.isObject()) continue;
    auto record = value.toObject().toVariantMap();
    if (record.value(QStringLiteral("id")).toString().trimmed().isEmpty() ||
        record.value(QStringLiteral("message")).toString().trimmed().isEmpty()) continue;
    record.insert(QStringLiteral("severity"), normalizeSeverity(record.value(QStringLiteral("severity")).toString()));
    record.insert(QStringLiteral("repeatCount"), std::max(1, record.value(QStringLiteral("repeatCount"), 1).toInt()));
    records_.append(record);
    if (!record.value(QStringLiteral("read")).toBool()) ++unread_count_;
  }
  trim();
  return true;
}

bool NotificationCenterFeature::save() const {
  const auto path = storagePath();
  const QFileInfo info{path};
  if (!QDir{}.mkpath(info.absolutePath())) return false;

  QJsonArray entries;
  for (const auto& value : records_) entries.append(toJson(value.toMap()));
  QJsonObject root{{QStringLiteral("schema"), QStringLiteral("xenon.launcher.notifications")},
                   {QStringLiteral("version"), 1},
                   {QStringLiteral("notifications"), entries}};

  QSaveFile file{path};
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
  if (file.write(QJsonDocument{root}.toJson(QJsonDocument::Indented)) < 0) return false;
  return file.commit();
}

}  // namespace xenon::launcher::frontend_backend
