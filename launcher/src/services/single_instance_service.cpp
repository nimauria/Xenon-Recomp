#include "single_instance_service.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>
#include <QThread>

namespace xenon::launcher {
namespace {

QByteArray serializeArguments(const QStringList& arguments) {
  QJsonArray payload;
  for (const auto& argument : arguments) payload.append(argument);
  auto bytes = QJsonDocument{payload}.toJson(QJsonDocument::Compact);
  bytes.append('\n');
  return bytes;
}

QStringList parseArguments(const QByteArray& payload) {
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(payload.trimmed(), &error);
  if (error.error != QJsonParseError::NoError || !document.isArray()) return {};

  QStringList result;
  for (const auto& value : document.array()) {
    if (result.size() >= 64) break;
    if (value.isString()) result.push_back(value.toString().left(8192));
  }
  return result;
}

}  // namespace

SingleInstanceService::SingleInstanceService(QObject* parent) : QObject(parent) {}
SingleInstanceService::~SingleInstanceService() = default;

QString SingleInstanceService::serverName() const {
  auto scope = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  if (scope.trimmed().isEmpty()) scope = QDir::homePath();
  const auto digest = QCryptographicHash::hash(scope.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
  return QStringLiteral("ProjectXenon.XenonLauncher.%1").arg(QString::fromLatin1(digest));
}

bool SingleInstanceService::forwardToExisting(const QStringList& arguments) const {
  QLocalSocket socket;
  socket.connectToServer(serverName(), QIODevice::ReadWrite);
  if (!socket.waitForConnected(350)) return false;

  const auto payload = serializeArguments(arguments);
  if (socket.write(payload) != payload.size()) return false;
  if (!socket.waitForBytesWritten(500)) return false;

  // Wait briefly for an acknowledgement so a shell-launched xenon:// URL is
  // not discarded before the primary process has actually accepted it.
  if (!socket.waitForReadyRead(650)) return false;
  return socket.readAll().startsWith("ok");
}

SingleInstanceService::StartResult SingleInstanceService::startOrForward(
    const QStringList& arguments, bool force_new_instance) {
  if (force_new_instance) return StartResult::Primary;
  if (forwardToExisting(arguments)) return StartResult::Forwarded;

  auto* server = new QLocalServer(this);
  server->setSocketOptions(QLocalServer::UserAccessOption);
  if (!server->listen(serverName())) {
    // Two launcher processes can race during startup. Give a newly-created
    // primary a moment to finish binding before considering the endpoint stale.
    QThread::msleep(80);
    if (forwardToExisting(arguments)) {
      delete server;
      return StartResult::Forwarded;
    }

    // A stale local-server endpoint can survive an abnormal process exit on
    // Unix-like hosts. Only remove it after repeated connection attempts fail.
    QLocalServer::removeServer(serverName());
    if (!server->listen(serverName())) {
      delete server;
      return StartResult::Unavailable;
    }
  }

  server_ = server;
  connect(server_, &QLocalServer::newConnection, this, &SingleInstanceService::acceptPendingConnections);
  return StartResult::Primary;
}

void SingleInstanceService::acceptPendingConnections() {
  while (server_ != nullptr && server_->hasPendingConnections()) {
    auto* socket = server_->nextPendingConnection();
    if (socket == nullptr) continue;

    auto* buffer = new QByteArray;
    connect(socket, &QLocalSocket::readyRead, socket, [this, socket, buffer]() {
      buffer->append(socket->readAll());
      if (buffer->size() > 64 * 1024) {
        socket->disconnectFromServer();
        return;
      }
      const auto newline = buffer->indexOf('\n');
      if (newline < 0) return;

      const auto payload = buffer->left(newline);
      const auto arguments = parseArguments(payload);
      emit argumentsReceived(arguments);
      socket->write("ok\n");
      socket->flush();
      socket->disconnectFromServer();
    });
    connect(socket, &QLocalSocket::disconnected, socket, [socket, buffer]() {
      delete buffer;
      socket->deleteLater();
    });
  }
}

}  // namespace xenon::launcher
