#pragma once

#include <QObject>
#include <QStringList>

class QLocalServer;

namespace xenon::launcher {

class SingleInstanceService final : public QObject {
  Q_OBJECT

 public:
  enum class StartResult {
    Primary,
    Forwarded,
    Unavailable,
  };

  explicit SingleInstanceService(QObject* parent = nullptr);
  ~SingleInstanceService() override;

  [[nodiscard]] StartResult startOrForward(const QStringList& arguments,
                                           bool force_new_instance = false);
  [[nodiscard]] QString serverName() const;

 signals:
  void argumentsReceived(const QStringList& arguments);

 private:
  [[nodiscard]] bool forwardToExisting(const QStringList& arguments) const;
  void acceptPendingConnections();

  QLocalServer* server_ = nullptr;
};

}  // namespace xenon::launcher
