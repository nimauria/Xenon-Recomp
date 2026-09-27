#include "frontend_backend/network/qt_network_transport.hpp"

#include "xenon/network/client.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <cassert>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>

namespace net = xenon::network;
using xenon::launcher::frontend_backend::QtNetworkTransport;

namespace {

net::NetworkConfig config_for(quint16 port) {
  net::NetworkConfig config{};
  config.enabled = true;
  config.environment = net::NetworkEnvironment::Development;
  config.endpoints.base_url =
      QStringLiteral("http://127.0.0.1:%1").arg(port).toStdString();
  config.connect_timeout = std::chrono::milliseconds(500);
  config.request_timeout = std::chrono::milliseconds(500);
  config.retry.maximum_retries = 0;
  return config;
}

net::NetworkClientIdentity identity() {
  net::NetworkClientIdentity result{};
  result.xenon_version = "qt-transport-test";
  result.host_platform = "test";
  result.host_architecture = "test";
  return result;
}

net::NetworkResult wait_for(std::function<void(net::NetworkTransport::Completion)> begin,
                            int timeout_ms = 3000) {
  QEventLoop loop;
  QTimer watchdog;
  watchdog.setSingleShot(true);
  net::NetworkResult result{};
  bool completed = false;
  QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
  begin([&](net::NetworkResult value) {
    result = std::move(value);
    completed = true;
    loop.quit();
  });
  watchdog.start(timeout_ms);
  loop.exec();
  assert(completed);
  return result;
}

void test_loopback_bootstrap_and_correlation() {
  QTcpServer server;
  assert(server.listen(QHostAddress::LocalHost, 0));
  QByteArray request_bytes;
  QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
    auto* socket = server.nextPendingConnection();
    QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &request_bytes]() {
      request_bytes.append(socket->readAll());
      if (!request_bytes.contains("\r\n\r\n")) return;
      const QByteArray body =
          R"({"protocolVersion":1,"minimumClientProtocol":1,"serviceVersion":"loopback","capabilities":["sessions"]})";
      socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " +
                    QByteArray::number(body.size()) + "\r\n\r\n" + body);
      socket->disconnectFromHost();
    });
  });

  auto transport = std::make_shared<QtNetworkTransport>();
  auto client = std::make_shared<net::XenonNetworkClient>(
      transport, config_for(server.serverPort()), identity());
  const auto result = wait_for([&](net::NetworkTransport::Completion completion) {
    client->start(std::move(completion));
  });
  assert(result.ok());
  assert(client->status().connection == net::NetworkConnectionState::Ready);
  assert(request_bytes.startsWith("GET /v1/bootstrap HTTP/1.1"));
  assert(request_bytes.toLower().contains("x-request-id:"));
  client->shutdown();
}

void test_connection_refused() {
  QTcpServer reserve;
  assert(reserve.listen(QHostAddress::LocalHost, 0));
  const auto unused_port = reserve.serverPort();
  reserve.close();

  auto transport = std::make_shared<QtNetworkTransport>();
  auto client = std::make_shared<net::XenonNetworkClient>(
      transport, config_for(unused_port), identity());
  const auto result = wait_for([&](net::NetworkTransport::Completion completion) {
    client->check_health(std::move(completion));
  });
  // Windows firewall policy may turn a closed loopback port into a bounded
  // connect timeout rather than an immediate RST. Both are truthful failures.
  assert(result.error.code == net::NetworkErrorCode::ConnectionFailure ||
         result.error.code == net::NetworkErrorCode::Timeout);
  client->shutdown();
}

void test_request_timeout() {
  QTcpServer server;
  assert(server.listen(QHostAddress::LocalHost, 0));
  QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
    auto* socket = server.nextPendingConnection();
    QObject::connect(socket, &QTcpSocket::readyRead, socket,
                     [socket]() { static_cast<void>(socket->readAll()); });
  });

  auto config = config_for(server.serverPort());
  config.request_timeout = std::chrono::milliseconds(100);
  auto transport = std::make_shared<QtNetworkTransport>();
  auto client =
      std::make_shared<net::XenonNetworkClient>(transport, config, identity());
  const auto result = wait_for([&](net::NetworkTransport::Completion completion) {
    client->check_health(std::move(completion));
  });
  assert(result.error.code == net::NetworkErrorCode::Timeout);
  client->shutdown();
}

void test_redirect_is_not_followed() {
  QTcpServer target;
  assert(target.listen(QHostAddress::LocalHost, 0));
  bool target_contacted = false;
  QObject::connect(&target, &QTcpServer::newConnection, &target, [&]() {
    target_contacted = true;
    if (auto* socket = target.nextPendingConnection()) socket->disconnectFromHost();
  });

  QTcpServer redirect;
  assert(redirect.listen(QHostAddress::LocalHost, 0));
  QObject::connect(&redirect, &QTcpServer::newConnection, &redirect, [&]() {
    auto* socket = redirect.nextPendingConnection();
    QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &target]() {
      static_cast<void>(socket->readAll());
      const auto location = QStringLiteral("http://127.0.0.1:%1/redirected")
                                .arg(target.serverPort())
                                .toLatin1();
      socket->write("HTTP/1.1 302 Found\r\nLocation: " + location +
                    "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
      socket->disconnectFromHost();
    });
  });

  auto transport = std::make_shared<QtNetworkTransport>();
  auto client = std::make_shared<net::XenonNetworkClient>(
      transport, config_for(redirect.serverPort()), identity());
  const auto result = wait_for([&](net::NetworkTransport::Completion completion) {
    client->check_health(std::move(completion));
  });
  assert(result.error.code == net::NetworkErrorCode::MalformedResponse);
  QCoreApplication::processEvents();
  assert(!target_contacted);
  client->shutdown();
}

void test_response_header_limit() {
  QTcpServer server;
  assert(server.listen(QHostAddress::LocalHost, 0));
  QObject::connect(&server, &QTcpServer::newConnection, &server, [&]() {
    auto* socket = server.nextPendingConnection();
    QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
      static_cast<void>(socket->readAll());
      QByteArray headers = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n";
      for (int i = 0; i < 70; ++i) {
        headers += "X-Fill-" + QByteArray::number(i) + ": value\r\n";
      }
      socket->write(headers + "\r\n{}");
      socket->disconnectFromHost();
    });
  });

  auto transport = std::make_shared<QtNetworkTransport>();
  auto client = std::make_shared<net::XenonNetworkClient>(
      transport, config_for(server.serverPort()), identity());
  const auto result = wait_for([&](net::NetworkTransport::Completion completion) {
    client->check_health(std::move(completion));
  });
  assert(result.error.code == net::NetworkErrorCode::ResponseTooLarge);
  client->shutdown();
}

void test_invalid_headers_and_cancelled_schedule() {
  auto transport = std::make_shared<QtNetworkTransport>();
  net::NetworkRequest request{};
  request.url = "http://127.0.0.1:1/v1/health";
  request.method = net::NetworkHttpMethod::Get;
  request.connect_timeout = std::chrono::milliseconds(100);
  request.request_timeout = std::chrono::milliseconds(100);
  request.headers.emplace("Authorization", "Bearer safe\r\nX-Injected: true");
  const auto invalid = wait_for([&](net::NetworkTransport::Completion completion) {
    transport->send(request, {}, std::move(completion));
  });
  assert(invalid.error.code == net::NetworkErrorCode::InvalidConfiguration);

  net::CancellationSource cancellation;
  QEventLoop loop;
  QTimer watchdog;
  watchdog.setSingleShot(true);
  bool task_ran = false;
  QObject::connect(&watchdog, &QTimer::timeout, &loop, &QEventLoop::quit);
  transport->schedule(std::chrono::seconds(5), cancellation.token(), [&]() {
    task_ran = true;
    loop.quit();
  });
  cancellation.cancel();
  watchdog.start(1000);
  loop.exec();
  assert(task_ran);
  transport->shutdown();
}

void test_plaintext_non_loopback_is_rejected() {
  auto transport = std::make_shared<QtNetworkTransport>();
  net::NetworkRequest request{};
  request.url = "http://example.com/v1/health";
  request.method = net::NetworkHttpMethod::Get;
  const auto result = wait_for([&](net::NetworkTransport::Completion completion) {
    transport->send(request, {}, std::move(completion));
  });
  assert(result.error.code == net::NetworkErrorCode::InvalidConfiguration);
  transport->shutdown();
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  std::cout << "Testing Qt Xenon Network transport...\n";
  test_loopback_bootstrap_and_correlation();
  test_connection_refused();
  test_request_timeout();
  test_redirect_is_not_followed();
  test_response_header_limit();
  test_invalid_headers_and_cancelled_schedule();
  test_plaintext_non_loopback_is_rejected();
  std::cout << "All Qt Xenon Network transport tests passed!\n";
  return 0;
}
