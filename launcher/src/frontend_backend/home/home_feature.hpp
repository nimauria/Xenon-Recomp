#pragma once

#include <QObject>
#include <QVariantMap>

namespace xenon::launcher::frontend_backend {
class LibraryFeature;
class ModulesFeature;
class NotificationCenterFeature;
class SessionController;

class HomeFeature final : public QObject {
  Q_OBJECT

 public:
  HomeFeature(LibraryFeature& library, ModulesFeature& modules,
              SessionController& session, NotificationCenterFeature& notifications,
              QObject* parent = nullptr);

  [[nodiscard]] QVariantMap snapshot() const;

 signals:
  void changed();

 private:
  LibraryFeature& library_;
  ModulesFeature& modules_;
  SessionController& session_;
  NotificationCenterFeature& notifications_;
};

}  // namespace xenon::launcher::frontend_backend
