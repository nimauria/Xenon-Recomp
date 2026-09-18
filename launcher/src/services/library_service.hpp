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
