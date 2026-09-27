#include "module_update_history_store.hpp"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace xenon::launcher::frontend_backend {
namespace {
constexpr qsizetype kMaxHistoryEntries = 100;
constexpr qsizetype kMaxPerModuleEntries = 20;
}

ModuleUpdateHistoryStore::ModuleUpdateHistoryStore() { load(); }

QString ModuleUpdateHistoryStore::storagePath() {
  const auto root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  return QDir{root}.filePath(QStringLiteral("module-updates/history.json"));
}

void ModuleUpdateHistoryStore::load() {
  entries_.clear();
  QFile file{storagePath()};
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text) || file.size() <= 0 ||
      file.size() > 2 * 1024 * 1024) {
    return;
  }
  QJsonParseError error{};
  const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !document.isObject()) return;
  const auto root = document.object();
  if (root.value(QStringLiteral("schema")).toString() !=
          QStringLiteral("xenon.module-update-history") ||
      root.value(QStringLiteral("version")).toInt() != 1) {
    return;
  }
  entries_ = root.value(QStringLiteral("entries")).toArray().toVariantList();
  prune();
}

bool ModuleUpdateHistoryStore::save() const {
  const QFileInfo info{storagePath()};
  if (!QDir{}.mkpath(info.absolutePath())) return false;

  QJsonObject root;
  root.insert(QStringLiteral("schema"), QStringLiteral("xenon.module-update-history"));
  root.insert(QStringLiteral("version"), 1);
  root.insert(QStringLiteral("entries"), QJsonArray::fromVariantList(entries_));

  QSaveFile file{storagePath()};
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
  if (file.write(QJsonDocument{root}.toJson(QJsonDocument::Indented)) < 0) return false;
  return file.commit();
}

void ModuleUpdateHistoryStore::prune() {
  QVariantList pruned;
  QHash<QString, int> per_module;
  for (const auto& value : entries_) {
    if (pruned.size() >= kMaxHistoryEntries) break;
    const auto item = value.toMap();
    const auto module_id = item.value(QStringLiteral("moduleId")).toString();
    if (module_id.isEmpty()) continue;
    const auto count = per_module.value(module_id, 0);
    if (count >= kMaxPerModuleEntries) continue;
    per_module.insert(module_id, count + 1);
    pruned.append(item);
  }
  entries_ = pruned;
}

QVariantList ModuleUpdateHistoryStore::entries(const QString& module_id) const {
  if (module_id.trimmed().isEmpty()) return entries_;
  QVariantList result;
  for (const auto& value : entries_) {
    const auto item = value.toMap();
    if (item.value(QStringLiteral("moduleId")).toString() == module_id) result.append(item);
  }
  return result;
}

bool ModuleUpdateHistoryStore::clear(const QString& module_id) {
  if (module_id.trimmed().isEmpty()) {
    entries_.clear();
    return save();
  }
  QVariantList remaining;
  for (const auto& value : entries_) {
    if (value.toMap().value(QStringLiteral("moduleId")).toString() != module_id) {
      remaining.append(value);
    }
  }
  entries_ = remaining;
  return save();
}

void ModuleUpdateHistoryStore::record(const QString& module_id, const QString& action,
                                      const QString& outcome, const QString& from_version,
                                      const QString& to_version, const QString& message,
                                      const QVariantMap& metadata) {
  if (module_id.trimmed().isEmpty()) return;
  QVariantMap entry = metadata;
  entry.insert(QStringLiteral("eventId"),
               QUuid::createUuid().toString(QUuid::WithoutBraces));
  entry.insert(QStringLiteral("moduleId"), module_id);
  entry.insert(QStringLiteral("action"), action);
  entry.insert(QStringLiteral("outcome"), outcome);
  entry.insert(QStringLiteral("fromVersion"), from_version);
  entry.insert(QStringLiteral("toVersion"), to_version);
  entry.insert(QStringLiteral("message"), message);
  entry.insert(QStringLiteral("timestamp"),
               QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  entries_.prepend(entry);
  prune();
  (void)save();
}

}  // namespace xenon::launcher::frontend_backend
