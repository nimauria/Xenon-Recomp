#pragma once

#include "../model/update_release.hpp"

#include <QObject>
#include <QString>

namespace xenon::launcher::frontend_backend {

class UpdateProvider : public QObject {
  Q_OBJECT

 public:
  explicit UpdateProvider(QObject* parent = nullptr) : QObject(parent) {}
  ~UpdateProvider() override = default;

  virtual void check(bool include_prerelease) = 0;
  virtual void download(const UpdateRelease& release, const QString& target_path) = 0;
  virtual void cancel() = 0;

 signals:
  void checkSucceeded(const xenon::launcher::frontend_backend::UpdateRelease& release);
  void checkFailed(const QString& message);
  void downloadProgress(qint64 received, qint64 total);
  void downloadSucceeded(const QString& target_path,
                         const xenon::launcher::frontend_backend::UpdateRelease& release);
  void downloadFailed(const QString& message);
};

}  // namespace xenon::launcher::frontend_backend
