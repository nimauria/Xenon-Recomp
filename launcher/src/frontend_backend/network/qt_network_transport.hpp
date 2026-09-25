#pragma once

#include <QList>
#include <QNetworkAccessManager>
#include <QPointer>

#include "xenon/network/transport.hpp"

class QNetworkReply;

namespace xenon::launcher::frontend_backend {

class QtNetworkTransport final : public QObject, public network::NetworkTransport {
  Q_OBJECT

 public:
  explicit QtNetworkTransport(QObject* parent = nullptr);
  ~QtNetworkTransport() override;

  [[nodiscard]] bool available() const noexcept override { return !shutting_down_; }
  void send(network::NetworkRequest request, network::CancellationToken cancellation,
            Completion completion) override;
  void schedule(std::chrono::milliseconds delay, network::CancellationToken cancellation,
                Task task) override;
  void shutdown() override;

 private:
  QNetworkAccessManager manager_;
  QList<QPointer<QNetworkReply>> replies_;
  bool shutting_down_{false};
};

}  // namespace xenon::launcher::frontend_backend
