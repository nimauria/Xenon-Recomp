#pragma once

#include "service_result.hpp"

#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

namespace xenon::launcher {

class PathService;

class LibraryService final : public QObject {
  Q_OBJECT

 public:
  explicit LibraryService(PathService& paths, QObject* parent = nullptr);

  [[nodiscard]] QVariantList entries() const;
  [[nodiscard]] QVariantMap entry(const QString& game_id) const;
  [[nodiscard]] ServiceResult registerIdentifiedContent(const QUrl& source,
                                                        const QVariantMap& identification);
  [[nodiscard]] ServiceResult remove(const QString& game_id);
  [[nodiscard]] ServiceResult setFavorite(const QString& game_id, bool favorite);
  // Destructive and deliberately separate from remove(): deletes the game's
  // Xenon-managed storage directory (managedPath(), which is always
  // constructed under the configured library "games" root - see
  // makeEntry()/registerIdentifiedContent() - so this can never reach
  // outside it or delete a source the user chose to "Keep in current
  // location"). Does not remove the library entry itself; call remove()
  // separately if that is also wanted. Fails cleanly, deleting nothing, if
  // the game has no managed storage to delete.
  [[nodiscard]] ServiceResult deleteManagedFiles(const QString& game_id) const;
  [[nodiscard]] ServiceResult verify(const QString& game_id) const;
  [[nodiscard]] ServiceResult applyIdentification(const QString& game_id,
                                                  const QVariantMap& identification);
  [[nodiscard]] bool markLaunched(const QString& game_id);
  [[nodiscard]] bool recordSessionEnded(const QString& game_id, qint64 duration_ms,
                                        const QString& outcome);
  [[nodiscard]] QString contentPath(const QString& game_id) const;
  [[nodiscard]] QString contentFolder(const QString& game_id) const;
  [[nodiscard]] QString managedPath(const QString& game_id) const;
  [[nodiscard]] QString ensureManagedPath(const QString& game_id) const;
  // Where a game's own disc/content media files live once moved into the
  // managed library ("Move into Xenon Library" on import - see
  // docs/development/GAME_PREPARATION.md "Managed game library"). Never used for content
  // left in its original location ("Keep in current location").
  [[nodiscard]] QString mediaDirectory(const QString& game_id) const;
  [[nodiscard]] QString ensureMediaDirectory(const QString& game_id) const;
  // Records that the media previously at `old_path` now lives at `new_path`
  // (a completed "Move into Xenon Library" import): updates the matching
  // media-list entry's path/importMode, and contentPath too if it was the
  // active disc. Fails if `old_path` is not a known media entry for this game.
  [[nodiscard]] ServiceResult relocateMedia(const QString& game_id, const QString& old_path,
                                            const QString& new_path);
  [[nodiscard]] QVariantMap properties(const QString& game_id) const;
  void reload();

 signals:
  void changed();

 private:
  [[nodiscard]] bool save() const;
  [[nodiscard]] QVariantMap makeEntry(const QString& local_path) const;

  PathService& paths_;
  QVariantList entries_;
};

}  // namespace xenon::launcher
