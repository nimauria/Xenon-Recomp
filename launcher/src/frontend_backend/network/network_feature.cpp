#include "network_feature.hpp"

#include "../../launcher_config.hpp"

#if XENON_LAUNCHER_RUNTIME_NETWORK
#include "qt_network_transport.hpp"

#include "xenon/core/run_fingerprint.hpp"
#include "xenon/network/client.hpp"
#endif

#include <QStringList>

#include <chrono>
#include <utility>

#ifndef XENON_LAUNCHER_VERSION
#define XENON_LAUNCHER_VERSION "0.0.0-dev"
#endif

namespace xenon::launcher::frontend_backend {

struct NetworkFeature::Implementation {
#if XENON_LAUNCHER_RUNTIME_NETWORK
  std::shared_ptr<QtNetworkTransport> transport{};
  std::shared_ptr<network::XenonNetworkClient> client{};
#endif
};

NetworkFeature::NetworkFeature(SettingsFeature& settings, QObject* parent)
    : QObject(parent), settings_(settings), implementation_(std::make_unique<Implementation>()) {
  snapshot_.insert(QStringLiteral("clientCompiled"),
                   static_cast<bool>(XENON_LAUNCHER_RUNTIME_NETWORK));
  snapshot_.insert(QStringLiteral("connectionState"), QStringLiteral("disabled"));
  snapshot_.insert(QStringLiteral("connectionLabel"), QStringLiteral("Disabled"));
  snapshot_.insert(QStringLiteral("onlineServicesReady"), false);
}

NetworkFeature::~NetworkFeature() { shutdown(); }

void NetworkFeature::initialize() { reconfigure(); }

void NetworkFeature::reconfigure() {
  shutdown();
#if XENON_LAUNCHER_RUNTIME_NETWORK
  network::NetworkConfig config{};
  config.enabled = settings_.boolValue(QStringLiteral("network/enabled"), false);
  const auto environment =
      settings_.stringValue(QStringLiteral("network/environment"), QStringLiteral("Offline"));
  if (environment.compare(QStringLiteral("Development"), Qt::CaseInsensitive) == 0) {
    config.environment = network::NetworkEnvironment::Development;
  } else if (environment.compare(QStringLiteral("Production"), Qt::CaseInsensitive) == 0) {
    config.environment = network::NetworkEnvironment::Production;
  } else {
    config.environment = network::NetworkEnvironment::Offline;
  }
  config.endpoints.base_url =
      settings_.stringValue(QStringLiteral("network/baseUrl")).trimmed().toStdString();
  config.connect_timeout = std::chrono::milliseconds(
      settings_.intValue(QStringLiteral("network/connectTimeoutMs"), 5000));
  config.request_timeout = std::chrono::milliseconds(
      settings_.intValue(QStringLiteral("network/requestTimeoutMs"), 10000));

  network::NetworkClientIdentity identity{};
  identity.xenon_version = XENON_LAUNCHER_VERSION;
  identity.host_platform = core::host_os_identifier();
  identity.host_architecture = core::host_cpu_arch_identifier();
  identity.requested_capabilities = {"authentication", "profile", "presence", "friends",
                                     "matchmaking", "sessions", "connectivity", "realtime"};

  implementation_->transport = std::make_shared<QtNetworkTransport>();
  implementation_->client = std::make_shared<network::XenonNetworkClient>(
      implementation_->transport, std::move(config), std::move(identity));
  implementation_->client->set_state_callback([this](const network::NetworkStatus& status) {
    const auto metrics = implementation_->client->metrics();
    QVariantMap value;
    value.insert(QStringLiteral("clientCompiled"), true);
    value.insert(QStringLiteral("transportAvailable"), status.transport_available);
    value.insert(QStringLiteral("configured"), status.configured);
    value.insert(QStringLiteral("serviceReachable"), status.service_reachable);
    value.insert(QStringLiteral("protocolCompatible"), status.protocol_compatible);
    value.insert(QStringLiteral("authenticated"),
                 status.authentication == network::NetworkAuthState::Authenticated);
    value.insert(QStringLiteral("realtimeActive"),
                 status.realtime == network::NetworkRealtimeState::Connected);
    value.insert(QStringLiteral("onlineServicesReady"),
                 status.connection == network::NetworkConnectionState::Ready);
    value.insert(QStringLiteral("connectionState"),
                 QString::fromLatin1(network::to_string(status.connection).data(),
                                     static_cast<qsizetype>(network::to_string(status.connection).size())));
    QString label;
    switch (status.connection) {
      case network::NetworkConnectionState::Offline: label = QStringLiteral("Offline"); break;
      case network::NetworkConnectionState::Disabled: label = QStringLiteral("Disabled"); break;
      case network::NetworkConnectionState::Resolving: label = QStringLiteral("Resolving"); break;
      case network::NetworkConnectionState::Connecting: label = QStringLiteral("Connecting"); break;
      case network::NetworkConnectionState::ConnectedTransport:
        label = QStringLiteral("Transport connected");
        break;
      case network::NetworkConnectionState::Authenticating:
        label = QStringLiteral("Authenticating");
        break;
      case network::NetworkConnectionState::Ready: label = QStringLiteral("Ready"); break;
      case network::NetworkConnectionState::Degraded: label = QStringLiteral("Degraded"); break;
      case network::NetworkConnectionState::Reconnecting:
        label = QStringLiteral("Reconnecting");
        break;
      case network::NetworkConnectionState::Unavailable:
        label = status.last_error.code == network::NetworkErrorCode::EndpointNotConfigured
                    ? QStringLiteral("Endpoint not configured")
                    : QStringLiteral("Service unavailable");
        break;
      case network::NetworkConnectionState::Error: label = QStringLiteral("Error"); break;
    }
    value.insert(QStringLiteral("connectionLabel"), label);
    value.insert(QStringLiteral("authenticationState"),
                 QString::fromStdString(std::string(network::to_string(status.authentication))));
    value.insert(QStringLiteral("realtimeState"),
                 QString::fromStdString(std::string(network::to_string(status.realtime))));
    value.insert(QStringLiteral("lastSuccessfulContact"),
                 QString::fromStdString(status.last_successful_contact));
    value.insert(QStringLiteral("lastError"),
                 QString::fromStdString(std::string(network::to_string(status.last_error.code))));
    value.insert(QStringLiteral("protocolVersion"),
                 static_cast<qulonglong>(network::kClientProtocolVersion));
    value.insert(QStringLiteral("baseEndpoint"),
                 QString::fromStdString(implementation_->client->config().endpoints.base_url));
    QStringList capabilities;
    for (const auto& capability : status.capabilities.negotiated) {
      capabilities.push_back(QString::fromStdString(capability));
    }
    value.insert(QStringLiteral("capabilities"), capabilities);
    value.insert(QStringLiteral("requestsAttempted"),
                 static_cast<qulonglong>(metrics.requests_attempted));
    value.insert(QStringLiteral("requestsFailed"),
                 static_cast<qulonglong>(metrics.requests_failed));
    publish(std::move(value));
  });
  implementation_->client->start();
#else
  publish({{QStringLiteral("clientCompiled"), false},
           {QStringLiteral("transportAvailable"), false},
           {QStringLiteral("configured"), false},
           {QStringLiteral("serviceReachable"), false},
           {QStringLiteral("protocolCompatible"), false},
           {QStringLiteral("authenticated"), false},
           {QStringLiteral("realtimeActive"), false},
           {QStringLiteral("onlineServicesReady"), false},
           {QStringLiteral("connectionState"), QStringLiteral("disabled")},
           {QStringLiteral("connectionLabel"), QStringLiteral("Not built")}});
#endif
}

void NetworkFeature::refreshHealth() {
#if XENON_LAUNCHER_RUNTIME_NETWORK
  if (implementation_->client) {
    implementation_->client->check_health([](network::NetworkResult) {});
  }
#endif
}

void NetworkFeature::shutdown() {
#if XENON_LAUNCHER_RUNTIME_NETWORK
  if (implementation_->client) implementation_->client->shutdown();
  implementation_->client.reset();
  implementation_->transport.reset();
#endif
}

QVariantMap NetworkFeature::snapshot() const { return snapshot_; }

void NetworkFeature::publish(QVariantMap snapshot) {
  snapshot_ = std::move(snapshot);
  emit changed();
}

}  // namespace xenon::launcher::frontend_backend
