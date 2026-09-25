#include "qt_network_transport.hpp"

#include <QByteArray>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

namespace xenon::launcher::frontend_backend {
namespace {

network::NetworkErrorCode map_error(QNetworkReply::NetworkError error) {
  switch (error) {
    case QNetworkReply::NoError: return network::NetworkErrorCode::None;
    case QNetworkReply::HostNotFoundError: return network::NetworkErrorCode::DnsFailure;
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::BackgroundRequestNotAllowedError:
      return network::NetworkErrorCode::ConnectionFailure;
    case QNetworkReply::SslHandshakeFailedError: return network::NetworkErrorCode::TlsFailure;
    case QNetworkReply::TimeoutError: return network::NetworkErrorCode::Timeout;
    case QNetworkReply::OperationCanceledError: return network::NetworkErrorCode::Cancelled;
    default: return network::NetworkErrorCode::ConnectionFailure;
  }
}

}  // namespace

QtNetworkTransport::QtNetworkTransport(QObject* parent) : QObject(parent), manager_(this) {}

QtNetworkTransport::~QtNetworkTransport() { shutdown(); }

void QtNetworkTransport::send(network::NetworkRequest request,
                              network::CancellationToken cancellation,
                              Completion completion) {
  if (shutting_down_ || cancellation.cancelled()) {
    completion({{}, {network::NetworkErrorCode::Cancelled, "Network transport is shutting down"}});
    return;
  }

  QNetworkRequest qt_request{QUrl(QString::fromStdString(request.url))};
  qt_request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                          QNetworkRequest::NoLessSafeRedirectPolicy);
  qt_request.setTransferTimeout(static_cast<int>(std::min<std::int64_t>(
      request.request_timeout.count(), std::numeric_limits<int>::max())));
  for (const auto& [name, value] : request.headers) {
    qt_request.setRawHeader(QByteArray::fromStdString(name), QByteArray::fromStdString(value));
  }

  const auto body = QByteArray::fromStdString(request.body);
  QNetworkReply* reply = nullptr;
  switch (request.method) {
    case network::NetworkHttpMethod::Get: reply = manager_.get(qt_request); break;
    case network::NetworkHttpMethod::Post: reply = manager_.post(qt_request, body); break;
    case network::NetworkHttpMethod::Put: reply = manager_.put(qt_request, body); break;
    case network::NetworkHttpMethod::Patch:
      reply = manager_.sendCustomRequest(qt_request, QByteArrayLiteral("PATCH"), body);
      break;
    case network::NetworkHttpMethod::Delete:
      reply = body.isEmpty()
                  ? manager_.deleteResource(qt_request)
                  : manager_.sendCustomRequest(qt_request, QByteArrayLiteral("DELETE"), body);
      break;
  }
  if (reply == nullptr) {
    completion({{}, {network::NetworkErrorCode::ConnectionFailure,
                     "Qt could not create the network request"}});
    return;
  }
  replies_.push_back(reply);

  auto response_body = std::make_shared<QByteArray>();
  auto abort_reason = std::make_shared<network::NetworkErrorCode>(network::NetworkErrorCode::None);
  auto connected = std::make_shared<bool>(false);

  auto* connect_timer = new QTimer(reply);
  connect_timer->setSingleShot(true);
  connect_timer->setInterval(static_cast<int>(std::min<std::int64_t>(
      request.connect_timeout.count(), std::numeric_limits<int>::max())));
  connect(connect_timer, &QTimer::timeout, reply, [reply, abort_reason, connected]() {
    if (*connected) return;
    *abort_reason = network::NetworkErrorCode::Timeout;
    reply->abort();
  });
  connect(reply, &QNetworkReply::metaDataChanged, reply, [connect_timer, connected]() {
    *connected = true;
    connect_timer->stop();
  });
  connect_timer->start();

  auto* cancellation_timer = new QTimer(reply);
  cancellation_timer->setInterval(50);
  connect(cancellation_timer, &QTimer::timeout, reply,
          [reply, cancellation, abort_reason]() {
            if (!cancellation.cancelled()) return;
            *abort_reason = network::NetworkErrorCode::Cancelled;
            reply->abort();
          });
  cancellation_timer->start();

  connect(reply, &QIODevice::readyRead, reply,
          [reply, response_body, abort_reason, maximum = request.maximum_response_bytes]() {
            const auto chunk = reply->readAll();
            if (static_cast<std::size_t>(response_body->size() + chunk.size()) > maximum) {
              *abort_reason = network::NetworkErrorCode::ResponseTooLarge;
              response_body->clear();
              reply->abort();
              return;
            }
            response_body->append(chunk);
          });

  connect(reply, &QNetworkReply::finished, this,
          [this, reply, response_body, abort_reason, request = std::move(request),
           completion = std::move(completion)]() mutable {
            replies_.removeAll(reply);
            if (*abort_reason == network::NetworkErrorCode::None) {
              const auto tail = reply->readAll();
              if (static_cast<std::size_t>(response_body->size() + tail.size()) >
                  request.maximum_response_bytes) {
                *abort_reason = network::NetworkErrorCode::ResponseTooLarge;
                response_body->clear();
              } else {
                response_body->append(tail);
              }
            }

            network::NetworkResult result{};
            result.response.status_code =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            result.response.body = response_body->toStdString();
            result.response.bytes_sent = request.body.size();
            result.response.bytes_received = static_cast<std::size_t>(response_body->size());
            for (const auto& header : reply->rawHeaderPairs()) {
              result.response.headers.emplace(header.first.toStdString(), header.second.toStdString());
            }

            auto code = *abort_reason;
            if (code == network::NetworkErrorCode::None &&
                result.response.status_code == 0 && reply->error() != QNetworkReply::NoError) {
              code = map_error(reply->error());
            }
            if (code != network::NetworkErrorCode::None) {
              result.error.code = code;
              result.error.diagnostic =
                  network::sanitize_for_log(reply->errorString().toStdString());
            }
            reply->deleteLater();
            completion(std::move(result));
          });
}

void QtNetworkTransport::schedule(std::chrono::milliseconds delay,
                                  network::CancellationToken cancellation, Task task) {
  QTimer::singleShot(static_cast<int>(std::min<std::int64_t>(
                         delay.count(), std::numeric_limits<int>::max())),
                     this, [cancellation, task = std::move(task)]() mutable {
                       if (!cancellation.cancelled()) task();
                     });
}

void QtNetworkTransport::shutdown() {
  if (shutting_down_) return;
  shutting_down_ = true;
  const auto replies = replies_;
  for (const auto& reply : replies) {
    if (reply) reply->abort();
  }
  replies_.clear();
}

}  // namespace xenon::launcher::frontend_backend
