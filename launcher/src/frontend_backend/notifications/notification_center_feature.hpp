#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include "../../services/service_result.hpp"

namespace xenon::launcher {
class PathService;
}

namespace xenon::launcher::frontend_backend {

class NotificationCenterFeature final : public QObject {
  Q_OBJECT

 public:
  explicit NotificationCenterFeature(PathService& paths, QObject* parent = nullptr);

  [[nodiscard]] QVariantList entries() const;
  [[nodiscard]] QVariantMap entry(const QString& notification_id) const;
  [[nodiscard]] int unreadCount() const noexcept;
  [[nodiscard]] QString storagePath() const;

  [[nodiscard]] ServiceResult add(const QString& title, const QString& message,
                                  const QString& severity = QStringLiteral("info"),
                                  const QString& source = QStringLiteral("launcher"),
                                  const QString& command_id = {},
                                  const QString& target_id = {},
                                  const QString& section_id = {},
                                  const QString& action_label = {});
  [[nodiscard]] ServiceResult markRead(const QString& notification_id, bool read = true);
  [[nodiscard]] ServiceResult markAllRead();
  [[nodiscard]] ServiceResult dismiss(const QString& notification_id);
  [[nodiscard]] ServiceResult clear();

 signals:
  void changed();

 private:
  [[nodiscard]] bool load();
  [[nodiscard]] bool save() const;
  [[nodiscard]] static QString normalizeSeverity(const QString& severity);
  [[nodiscard]] static QVariantMap publicRecord(const QVariantMap& record);
  void trim();

  PathService& paths_;
  QVariantList records_;
  int unread_count_ = 0;
};

}  // namespace xenon::launcher::frontend_backend
