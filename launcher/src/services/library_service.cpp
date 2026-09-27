#include "library_service.hpp"

#include "path_service.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QRegularExpression>

namespace xenon::launcher {
namespace {
QString normalizedPath(const QString& path) {
  QFileInfo info{path};
  auto normalized = info.exists() ? info.canonicalFilePath() : info.absoluteFilePath();
  if (normalized.isEmpty()) normalized = QDir::cleanPath(path);
#if defined(Q_OS_WIN)
  normalized = normalized.toLower();
#endif
  return normalized;
}


QString safeLibraryId(QString value) {
  value = value.trimmed();
  value.replace(QRegularExpression{QStringLiteral("[^A-Za-z0-9._-]")}, QStringLiteral("_"));
  if (value.isEmpty() || value == QStringLiteral(".") || value == QStringLiteral(".."))
    return QStringLiteral("game");
  return value.left(128);
}

QString stableLocalId(const QString& path) {
  const auto digest = QCryptographicHash::hash(normalizedPath(path).toUtf8(),
                                               QCryptographicHash::Sha256).toHex();
  return QStringLiteral("local-%1").arg(QString::fromLatin1(digest.left(16)));
}

// The library's primary key is derived from the game's own canonical
// identity (its Title ID, from real XEX parsing - never a filename or
// folder name) whenever identification supplies one, so the SAME title
// imported from a different disc copy, a different region's media, or a
// moved file always resolves to the SAME managed library entry instead of
// being duplicated. Falls back to a path-derived id only when no title
// identity is available at all (should not happen for a successfully
// identified XEX/GDFX/STFS source - see xenon::filesystem::ContentProbe).
QString canonicalGameId(const QVariantMap& identification, const QString& fallback_local_path) {
  const auto title_id = identification.value(QStringLiteral("titleId")).toString().trimmed();
  if (!title_id.isEmpty()) return QStringLiteral("title-%1").arg(title_id.toLower());
  return stableLocalId(fallback_local_path);
}

QVariantMap mediaRecord(const QVariantMap& identification, const QString& local_path, bool is_primary) {
  QVariantMap record;
  record.insert(QStringLiteral("mediaId"), identification.value(QStringLiteral("mediaId")));
  record.insert(QStringLiteral("discNumber"), identification.value(QStringLiteral("discNumber")));
  record.insert(QStringLiteral("discCount"), identification.value(QStringLiteral("discCount")));
  record.insert(QStringLiteral("sourceType"), identification.value(QStringLiteral("sourceType")));
  record.insert(QStringLiteral("sourcePath"), QFileInfo{local_path}.absoluteFilePath());
  record.insert(QStringLiteral("importMode"), QStringLiteral("kept"));
  record.insert(QStringLiteral("isPrimary"), is_primary);
  record.insert(QStringLiteral("addedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  return record;
}
}

LibraryService::LibraryService(PathService& paths, QObject* parent)
    : QObject(parent), paths_(paths) {}

QVariantList LibraryService::entries() const {
  return entries_;
}

QVariantMap LibraryService::entry(const QString& game_id) const {
  for (const auto& value : entries_) {
    const auto item = value.toMap();
    if (item.value(QStringLiteral("gameId")).toString() == game_id) return item;
  }
  return {};
}

ServiceResult LibraryService::registerIdentifiedContent(const QUrl& source,
                                                        const QVariantMap& identification) {
  if (!source.isLocalFile()) {
    return ServiceResult::failure(QStringLiteral("Library import"),
                                  QStringLiteral("Only local game content can be registered."));
  }
  const auto local = source.toLocalFile();
  const QFileInfo info{local};
  if (!info.exists()) {
    return ServiceResult::failure(QStringLiteral("Library import"),
                                  QStringLiteral("The selected game content no longer exists."));
  }

  // A valid, identified Xbox 360 title/disc can be registered even when no
  // installed module supports it yet (Part 7 of the automatic game
  // preparation pass): moduleId may legitimately be empty here. Module
  // resolution can happen later - installing a compatible module, or
  // re-running IContentProbe::matchModule() against the stored identity when
  // the user presses Play - via applyIdentification().
  const auto module_id = identification.value(QStringLiteral("moduleId")).toString().trimmed();
  const auto title_id = identification.value(QStringLiteral("titleId")).toString().trimmed();

  const auto normalized = normalizedPath(local);

  // 1. The exact same source path is already registered - refresh it in place.
  for (const auto& value : entries_) {
    const auto existing = value.toMap();
    if (normalizedPath(existing.value(QStringLiteral("contentPath")).toString()) != normalized) continue;
    const auto game_id = existing.value(QStringLiteral("gameId")).toString();
    const auto updated = applyIdentification(game_id, identification);
    if (!updated.ok) return updated;
    return ServiceResult::success(QStringLiteral("Library entry updated"),
                                  QStringLiteral("The existing library entry was refreshed with the content-probe result."),
                                  game_id);
  }

  // 2. A different source path (a different disc/region, or the same disc
  //    moved/copied elsewhere) resolves to the SAME title identity as an
  //    already-registered entry - merge as an additional media entry rather
  //    than creating a duplicate game entry (a title must not be duplicated
  //    merely because a second region/disc was imported).
  if (!title_id.isEmpty()) {
    for (const auto& value : entries_) {
      const auto existing = value.toMap();
      if (existing.value(QStringLiteral("titleId")).toString().trimmed() != title_id) continue;
      const auto game_id = existing.value(QStringLiteral("gameId")).toString();

      // Only apply the rest of the identification (module/display fields) if
      // this entry doesn't already have a module assigned - never clobber an
      // existing, working match with a fresh probe of a different disc.
      if (existing.value(QStringLiteral("moduleId")).toString().trimmed().isEmpty() &&
          !module_id.isEmpty()) {
        const auto updated = applyIdentification(game_id, identification);
        if (!updated.ok) return updated;
      }

      auto media = entry(game_id).value(QStringLiteral("media")).toList();
      bool already_known = false;
      for (const auto& media_value : media) {
        if (normalizedPath(media_value.toMap().value(QStringLiteral("sourcePath")).toString()) == normalized) {
          already_known = true;
          break;
        }
      }
      if (!already_known) {
        media.append(mediaRecord(identification, local, /*is_primary=*/false));
        for (qsizetype i = 0; i < entries_.size(); ++i) {
          if (entries_.at(i).toMap().value(QStringLiteral("gameId")).toString() != game_id) continue;
          auto updated_entry = entries_.at(i).toMap();
          updated_entry.insert(QStringLiteral("media"), media);
          entries_[i] = updated_entry;
          break;
        }
        if (!save()) {
          return ServiceResult::failure(
              QStringLiteral("Library import"),
              QStringLiteral("Xenon recognized this as another disc of an existing library entry, "
                             "but could not save the updated library metadata."));
        }
        emit changed();
      }
      return ServiceResult::success(
          QStringLiteral("Media added to existing title"),
          QStringLiteral("This disc/region was added to the existing library entry for the same title."),
          game_id);
    }
  }

  // 3. A genuinely new title.
  const auto game_id = canonicalGameId(identification, local);
  auto item = makeEntry(local);
  item.insert(QStringLiteral("gameId"), game_id);
  item.insert(QStringLiteral("managedPath"),
              QDir{paths_.configuredPath(QStringLiteral("games"))}.filePath(safeLibraryId(game_id)));
  item.insert(QStringLiteral("media"), QVariantList{mediaRecord(identification, local, /*is_primary=*/true)});
  item.insert(QStringLiteral("moduleId"), module_id);
  static const QStringList identified_fields{
      QStringLiteral("title"), QStringLiteral("moduleName"), QStringLiteral("moduleVersion"),
      QStringLiteral("tileArt"), QStringLiteral("heroArt"), QStringLiteral("description"),
      QStringLiteral("tileArtFocalX"), QStringLiteral("tileArtFocalY"),
      QStringLiteral("heroArtFocalX"), QStringLiteral("heroArtFocalY"),
      QStringLiteral("renderer"), QStringLiteral("mode"), QStringLiteral("regions"),
      QStringLiteral("contentState"), QStringLiteral("tags"), QStringLiteral("titleId"),
      QStringLiteral("mediaId"), QStringLiteral("xexVersion"), QStringLiteral("discNumber"),
      QStringLiteral("discCount"), QStringLiteral("sourceType"), QStringLiteral("executablePath")};
  for (const auto& field : identified_fields) {
    if (identification.contains(field)) item.insert(field, identification.value(field));
  }
  item.insert(QStringLiteral("ready"), identification.value(QStringLiteral("ready"), true).toBool());
  item.insert(QStringLiteral("status"),
              identification.value(QStringLiteral("status"), QStringLiteral("Ready")));
  item.insert(QStringLiteral("identifiedAt"),
              QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));

  entries_.append(item);
  if (!save()) {
    entries_.removeLast();
    return ServiceResult::failure(QStringLiteral("Library import"),
                                  QStringLiteral("Xenon identified the game but could not save the library metadata."));
  }
  emit changed();
  return ServiceResult::success(QStringLiteral("Game added to Library"),
                                QStringLiteral("%1 was identified and registered with %2.")
                                    .arg(item.value(QStringLiteral("title")).toString(),
                                         item.value(QStringLiteral("moduleName"), module_id).toString()),
                                item.value(QStringLiteral("gameId")));
}

ServiceResult LibraryService::remove(const QString& game_id) {
  for (qsizetype i = 0; i < entries_.size(); ++i) {
    if (entries_.at(i).toMap().value(QStringLiteral("gameId")).toString() != game_id) continue;
    const auto removed = entries_.takeAt(i).toMap();
    if (!save()) {
      entries_.insert(i, removed);
      return ServiceResult::failure(QStringLiteral("Library error"),
                                    QStringLiteral("Xenon could not save the library after removing the entry."));
    }
    emit changed();
    return ServiceResult::success(QStringLiteral("Removed from Library"),
                                  QStringLiteral("%1 was removed from the launcher library. No game files were deleted.")
                                      .arg(removed.value(QStringLiteral("title")).toString()));
  }
  return ServiceResult::failure(QStringLiteral("Library error"),
                                QStringLiteral("The selected library entry no longer exists."));
}

ServiceResult LibraryService::setFavorite(const QString& game_id, bool favorite) {
  for (qsizetype i = 0; i < entries_.size(); ++i) {
    auto current = entries_.at(i).toMap();
    if (current.value(QStringLiteral("gameId")).toString() != game_id) continue;
    const auto previous = current;
    current.insert(QStringLiteral("favorite"), favorite);
    entries_[i] = current;
    if (!save()) {
      entries_[i] = previous;
      return ServiceResult::failure(QStringLiteral("Favorite"),
                                    QStringLiteral("Xenon could not save the favorite state for this game."));
    }
    emit changed();
    return ServiceResult::success(
        favorite ? QStringLiteral("Added to favorites") : QStringLiteral("Removed from favorites"),
        current.value(QStringLiteral("title"), QStringLiteral("Game")).toString());
  }
  return ServiceResult::failure(QStringLiteral("Favorite"),
                                QStringLiteral("The selected library entry no longer exists."));
}

ServiceResult LibraryService::deleteManagedFiles(const QString& game_id) const {
  const auto item = entry(game_id);
  if (item.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Delete managed files"),
                                  QStringLiteral("The selected library entry no longer exists."));
  }
  const auto target = QDir::cleanPath(managedPath(game_id));
  const auto games_root = QDir::cleanPath(paths_.configuredPath(QStringLiteral("games")));
  // Defense in depth on top of managedPath()'s own construction: refuse to
  // touch anything that is not actually inside the managed library "games"
  // root, so this can never be tricked into deleting a "Keep in current
  // location" source or anything else Xenon does not own.
  if (target.isEmpty() || games_root.isEmpty() ||
      !(target == games_root || target.startsWith(games_root + QStringLiteral("/")))) {
    return ServiceResult::failure(QStringLiteral("Delete managed files"),
                                  QStringLiteral("This game has no Xenon-managed storage Xenon can safely delete."));
  }
  QDir directory{target};
  if (!directory.exists()) {
    return ServiceResult::failure(QStringLiteral("Delete managed files"),
                                  QStringLiteral("This game has no Xenon-managed storage to delete."));
  }
  if (!directory.removeRecursively()) {
    return ServiceResult::failure(QStringLiteral("Delete managed files"),
                                  QStringLiteral("Xenon could not delete all managed files for this game. "
                                                 "Some files may be open or in use."));
  }
  const auto title = item.value(QStringLiteral("title"), QStringLiteral("This game")).toString();
  return ServiceResult::success(QStringLiteral("Managed files deleted"),
                                QStringLiteral("%1's Xenon-managed files were permanently deleted.").arg(title));
}

ServiceResult LibraryService::verify(const QString& game_id) const {
  const auto item = entry(game_id);
  if (item.isEmpty()) {
    return ServiceResult::failure(QStringLiteral("Verification failed"),
                                  QStringLiteral("The selected library entry no longer exists."));
  }
  const auto path = item.value(QStringLiteral("contentPath")).toString();
  const QFileInfo info{path};
  if (!info.exists()) {
    return ServiceResult::failure(QStringLiteral("Content missing"),
                                  QStringLiteral("The registered game content path no longer exists:\n%1").arg(path));
  }
  return ServiceResult::success(
      QStringLiteral("Content path verified"),
      QStringLiteral("The registered path exists. Full Xbox 360 content validation is delegated to the selected game module."));
}

ServiceResult LibraryService::applyIdentification(const QString& game_id,
                                                   const QVariantMap& identification) {
  // moduleId may legitimately be empty: this refreshes/attaches whatever the
  // identification result knows (content identity fields, and a module when
  // one was matched) without requiring a module to already be assigned - see
  // registerIdentifiedContent()'s matching comment.
  const auto module_id = identification.value(QStringLiteral("moduleId")).toString().trimmed();

  for (qsizetype i = 0; i < entries_.size(); ++i) {
    auto current = entries_.at(i).toMap();
    if (current.value(QStringLiteral("gameId")).toString() != game_id) continue;

    const auto previous = current;
    static const QStringList mutable_fields{
        QStringLiteral("title"), QStringLiteral("moduleName"), QStringLiteral("moduleVersion"),
        QStringLiteral("tileArt"), QStringLiteral("heroArt"), QStringLiteral("description"),
        QStringLiteral("tileArtFocalX"), QStringLiteral("tileArtFocalY"),
        QStringLiteral("heroArtFocalX"), QStringLiteral("heroArtFocalY"),
        QStringLiteral("renderer"), QStringLiteral("mode"), QStringLiteral("regions"),
        QStringLiteral("contentState"), QStringLiteral("tags"), QStringLiteral("titleId"),
      QStringLiteral("mediaId"), QStringLiteral("xexVersion"), QStringLiteral("discNumber"),
      QStringLiteral("discCount"), QStringLiteral("sourceType"), QStringLiteral("executablePath")};
    for (const auto& field : mutable_fields) {
      if (identification.contains(field)) current.insert(field, identification.value(field));
    }
    current.insert(QStringLiteral("moduleId"), module_id);
    current.insert(QStringLiteral("ready"), identification.value(QStringLiteral("ready"), true).toBool());
    current.insert(QStringLiteral("status"),
                   identification.value(QStringLiteral("status"), QStringLiteral("Ready")));
    current.insert(QStringLiteral("identifiedAt"),
                   QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    entries_[i] = current;

    if (!save()) {
      entries_[i] = previous;
      return ServiceResult::failure(QStringLiteral("Content identification"),
                                    QStringLiteral("Xenon identified the content but could not save the updated library metadata."));
    }
    emit changed();
    return ServiceResult::success(QStringLiteral("Content identified"),
                                  QStringLiteral("%1 is now associated with %2.")
                                      .arg(current.value(QStringLiteral("title")).toString(),
                                           current.value(QStringLiteral("moduleName"), module_id).toString()));
  }

  return ServiceResult::failure(QStringLiteral("Content identification"),
                                QStringLiteral("The library entry no longer exists."));
}

bool LibraryService::markLaunched(const QString& game_id) {
  for (qsizetype i = 0; i < entries_.size(); ++i) {
    auto current = entries_.at(i).toMap();
    if (current.value(QStringLiteral("gameId")).toString() != game_id) continue;
    const auto previous = current;
    current.insert(QStringLiteral("lastPlayed"), QStringLiteral("Just now"));
    current.insert(QStringLiteral("lastPlayedAt"),
                   QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    current.insert(QStringLiteral("playCount"),
                   current.value(QStringLiteral("playCount"), 0).toLongLong() + 1);
    entries_[i] = current;
    if (!save()) {
      entries_[i] = previous;
      return false;
    }
    emit changed();
    return true;
  }
  return false;
}

bool LibraryService::recordSessionEnded(const QString& game_id, qint64 duration_ms,
                                        const QString& outcome) {
  for (qsizetype i = 0; i < entries_.size(); ++i) {
    auto current = entries_.at(i).toMap();
    if (current.value(QStringLiteral("gameId")).toString() != game_id) continue;
    const auto previous = current;
    const auto safe_duration = qMax<qint64>(0, duration_ms);
    current.insert(QStringLiteral("totalPlayTimeMs"),
                   current.value(QStringLiteral("totalPlayTimeMs"), 0).toLongLong() + safe_duration);
    current.insert(QStringLiteral("lastSessionDurationMs"), safe_duration);
    current.insert(QStringLiteral("lastSessionOutcome"), outcome.trimmed());
    current.insert(QStringLiteral("lastSessionEndedAt"),
                   QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    entries_[i] = current;
    if (!save()) {
      entries_[i] = previous;
      return false;
    }
    emit changed();
    return true;
  }
  return false;
}

QString LibraryService::contentPath(const QString& game_id) const {
  return entry(game_id).value(QStringLiteral("contentPath")).toString();
}

QString LibraryService::contentFolder(const QString& game_id) const {
  const QFileInfo info{contentPath(game_id)};
  if (!info.exists()) return {};
  return info.isDir() ? info.absoluteFilePath() : info.absolutePath();
}

QString LibraryService::managedPath(const QString& game_id) const {
  const auto item = entry(game_id);
  if (item.isEmpty()) return {};
  const auto stored = item.value(QStringLiteral("managedPath")).toString().trimmed();
  if (!stored.isEmpty()) return QDir::cleanPath(stored);
  return QDir{paths_.configuredPath(QStringLiteral("games"))}.filePath(safeLibraryId(game_id));
}

QString LibraryService::ensureManagedPath(const QString& game_id) const {
  const auto path = managedPath(game_id);
  return !path.isEmpty() && paths_.ensureDirectory(path) ? path : QString{};
}

QString LibraryService::mediaDirectory(const QString& game_id) const {
  const auto managed = managedPath(game_id);
  return managed.isEmpty() ? QString{} : QDir{managed}.filePath(QStringLiteral("Media"));
}

QString LibraryService::ensureMediaDirectory(const QString& game_id) const {
  const auto path = mediaDirectory(game_id);
  return !path.isEmpty() && paths_.ensureDirectory(path) ? path : QString{};
}

ServiceResult LibraryService::relocateMedia(const QString& game_id, const QString& old_path,
                                            const QString& new_path) {
  const auto old_normalized = normalizedPath(old_path);
  for (qsizetype i = 0; i < entries_.size(); ++i) {
    auto current = entries_.at(i).toMap();
    if (current.value(QStringLiteral("gameId")).toString() != game_id) continue;
    const auto previous = current;

    auto media = current.value(QStringLiteral("media")).toList();
    bool found = false;
    for (auto& media_value : media) {
      auto record = media_value.toMap();
      if (normalizedPath(record.value(QStringLiteral("sourcePath")).toString()) != old_normalized) continue;
      record.insert(QStringLiteral("sourcePath"), QFileInfo{new_path}.absoluteFilePath());
      record.insert(QStringLiteral("importMode"), QStringLiteral("moved"));
      media_value = record;
      found = true;
      break;
    }
    if (!found) {
      return ServiceResult::failure(
          QStringLiteral("Library import"),
          QStringLiteral("The moved file was not a known media entry for this library entry."));
    }
    current.insert(QStringLiteral("media"), media);
    if (normalizedPath(current.value(QStringLiteral("contentPath")).toString()) == old_normalized) {
      current.insert(QStringLiteral("contentPath"), QFileInfo{new_path}.absoluteFilePath());
    }
    entries_[i] = current;
    if (!save()) {
      entries_[i] = previous;
      return ServiceResult::failure(
          QStringLiteral("Library import"),
          QStringLiteral("The file was moved, but Xenon could not save the updated library metadata."));
    }
    emit changed();
    return ServiceResult::success(QStringLiteral("Moved into Xenon Library"),
                                  QStringLiteral("The content was moved into the managed library."));
  }
  return ServiceResult::failure(QStringLiteral("Library import"),
                                QStringLiteral("The library entry no longer exists."));
}

QVariantMap LibraryService::properties(const QString& game_id) const {
  auto item = entry(game_id);
  if (item.isEmpty()) return {};

  const QFileInfo content_info{item.value(QStringLiteral("contentPath")).toString()};
  const auto managed = managedPath(game_id);
  item.insert(QStringLiteral("contentExists"), content_info.exists());
  item.insert(QStringLiteral("contentIsDirectory"), content_info.exists() && content_info.isDir());
  item.insert(QStringLiteral("contentFolder"), contentFolder(game_id));
  item.insert(QStringLiteral("managedPath"), managed);
  item.insert(QStringLiteral("dlcRoot"), managed.isEmpty() ? QString{} : QDir{managed}.filePath(QStringLiteral("DLC")));
  item.insert(QStringLiteral("managedPathExists"), !managed.isEmpty() && QDir{managed}.exists());
  return item;
}

void LibraryService::reload() {
  entries_.clear();
  QFile file{paths_.libraryMetadataPath()};
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;

  QJsonParseError error{};
  const auto document = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError || !document.isArray()) return;
  bool migrated = false;
  for (const auto& value : document.array()) {
    if (!value.isObject()) continue;
    auto item = value.toObject().toVariantMap();
    const auto game_id = item.value(QStringLiteral("gameId")).toString();
    if (!game_id.isEmpty() && item.value(QStringLiteral("managedPath")).toString().trimmed().isEmpty()) {
      item.insert(QStringLiteral("managedPath"),
                  QDir{paths_.configuredPath(QStringLiteral("games"))}.filePath(safeLibraryId(game_id)));
      migrated = true;
    }
    entries_.append(item);
  }
  if (migrated) (void)save();
}

bool LibraryService::save() const {
  const QFileInfo target_info{paths_.libraryMetadataPath()};
  if (!paths_.ensureDirectory(target_info.absolutePath())) return false;

  QJsonArray array;
  for (const auto& value : entries_) array.append(QJsonObject::fromVariantMap(value.toMap()));

  QSaveFile file{target_info.absoluteFilePath()};
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
  file.write(QJsonDocument{array}.toJson(QJsonDocument::Indented));
  return file.commit();
}

QVariantMap LibraryService::makeEntry(const QString& local_path) const {
  const QFileInfo info{local_path};
  auto title = info.isDir() ? info.fileName() : info.completeBaseName();
  if (title.trimmed().isEmpty()) title = info.fileName();
  const auto game_id = stableLocalId(local_path);

  QVariantMap item;
  item.insert(QStringLiteral("title"), title);
  item.insert(QStringLiteral("moduleName"), QStringLiteral("Unassigned"));
  item.insert(QStringLiteral("status"), QStringLiteral("Module required"));
  item.insert(QStringLiteral("ready"), false);
  item.insert(QStringLiteral("installed"), true);
  item.insert(QStringLiteral("favorite"), false);
  item.insert(QStringLiteral("tileArt"), QString{});
  item.insert(QStringLiteral("heroArt"), QString{});
  // Default to center; a matched module's manifest may narrow this via
  // insertFocalPoint() in content_probe.cpp once identification supplies it.
  item.insert(QStringLiteral("tileArtFocalX"), 0.5);
  item.insert(QStringLiteral("tileArtFocalY"), 0.5);
  item.insert(QStringLiteral("heroArtFocalX"), 0.5);
  item.insert(QStringLiteral("heroArtFocalY"), 0.5);
  item.insert(QStringLiteral("description"),
              QStringLiteral("Local Xbox 360 content registered with Xenon. Install or enable a compatible game module to identify and validate it."));
  item.insert(QStringLiteral("gameId"), game_id);
  item.insert(QStringLiteral("moduleId"), QString{});
  item.insert(QStringLiteral("renderer"), QStringLiteral("Automatic"));
  item.insert(QStringLiteral("mode"), QStringLiteral("Offline"));
  item.insert(QStringLiteral("regions"), QStringLiteral("Module-defined"));
  item.insert(QStringLiteral("contentState"), QStringLiteral("Awaiting module identification"));
  item.insert(QStringLiteral("tags"), QString{});
  item.insert(QStringLiteral("moduleVersion"), QString{});
  item.insert(QStringLiteral("lastPlayed"), QStringLiteral("Not launched"));
  item.insert(QStringLiteral("lastPlayedAt"), QString{});
  item.insert(QStringLiteral("playCount"), 0);
  item.insert(QStringLiteral("totalPlayTimeMs"), 0);
  item.insert(QStringLiteral("lastSessionDurationMs"), 0);
  item.insert(QStringLiteral("lastSessionOutcome"), QString{});
  item.insert(QStringLiteral("lastSessionEndedAt"), QString{});
  item.insert(QStringLiteral("contentPath"), info.absoluteFilePath());
  item.insert(QStringLiteral("managedPath"),
              QDir{paths_.configuredPath(QStringLiteral("games"))}.filePath(safeLibraryId(game_id)));
  item.insert(QStringLiteral("addedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
  return item;
}

}  // namespace xenon::launcher
