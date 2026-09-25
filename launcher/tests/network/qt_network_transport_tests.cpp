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
  assert(result.error.code == net::NetworkErrorCode::ConnectionFailure);
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

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  std::cout << "Testing Qt Xenon Network transport...\n";
  test_loopback_bootstrap_and_correlation();
  test_connection_refused();
  test_request_timeout();
  std::cout << "All Qt Xenon Network transport tests passed!\n";
  return 0;
}
