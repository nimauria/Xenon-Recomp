#include "xenon/network/client.hpp"
#include "xenon/network/realtime.hpp"
#include "xenon/network/xbox_services.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace net = xenon::network;

namespace {

std::string fixture(const char* name) {
  const auto path = std::filesystem::path(XENON_NETWORK_FIXTURE_DIR) / "v1" / name;
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

net::NetworkResponse response(int status, std::string body = {}) {
  net::NetworkResponse value{};
  value.status_code = status;
  value.bytes_received = body.size();
  value.body = std::move(body);
  return value;
}

class ScriptedTransport final : public net::NetworkTransport {
 public:
  bool available() const noexcept override { return available_; }

  void send(net::NetworkRequest request, net::CancellationToken cancellation,
            Completion completion) override {
    requests.push_back(std::move(request));
    if (cancellation.cancelled()) {
      completion(net::NetworkResult{{}, {net::NetworkErrorCode::Cancelled, "cancelled"}});
      return;
    }
    assert(next_result < results.size());
    completion(results[next_result++]);
  }

  void schedule(std::chrono::milliseconds delay, net::CancellationToken cancellation,
                Task task) override {
    scheduled_delays.push_back(delay);
    if (!cancellation.cancelled()) task();
  }

  void shutdown() override { shutdown_called = true; }

  bool available_{true};
  bool shutdown_called{false};
  std::size_t next_result{0};
  std::vector<net::NetworkResult> results{};
  std::vector<net::NetworkRequest> requests{};
  std::vector<std::chrono::milliseconds> scheduled_delays{};
};

class HoldingTransport final : public net::NetworkTransport {
 public:
  bool available() const noexcept override { return true; }
  void send(net::NetworkRequest, net::CancellationToken, Completion completion) override {
    completion_ = std::move(completion);
  }
  void schedule(std::chrono::milliseconds, net::CancellationToken, Task task) override {
    task_ = std::move(task);
  }
  void shutdown() override { shutdown_called = true; }

  Completion completion_{};
  Task task_{};
  bool shutdown_called{false};
};

class ScriptedRealtimeChannel final : public net::NetworkRealtimeChannel {
 public:
  void connect(std::string, std::string, net::CancellationToken cancellation,
               StateCallback state_callback, EventCallback event_callback) override {
    ++connect_count;
    if (cancellation.cancelled()) return;
    event_callback_ = std::move(event_callback);
    assert(next_state < states.size());
    const auto state = states[next_state++];
    state_callback(state, state == net::NetworkRealtimeState::Error
                              ? net::NetworkError{net::NetworkErrorCode::ConnectionFailure,
                                                  "realtime dropped"}
                              : net::NetworkError{});
  }
  void disconnect() override { disconnected = true; }
  void emit(net::NetworkRealtimeEvent event) {
    if (event_callback_) event_callback_(std::move(event));
  }

  std::vector<net::NetworkRealtimeState> states{};
  std::size_t next_state{0};
  int connect_count{0};
  bool disconnected{false};
  EventCallback event_callback_{};
};

net::NetworkConfig development_config() {
  net::NetworkConfig config{};
  config.enabled = true;
  config.environment = net::NetworkEnvironment::Development;
  config.endpoints.base_url = "http://127.0.0.1:38100/";
  config.retry.initial_backoff = std::chrono::milliseconds(0);
  config.retry.maximum_backoff = std::chrono::milliseconds(0);
  config.retry.jitter_ratio = 0.0;
  return config;
}

net::NetworkConfig development_config_with_service_origins() {
  auto config = development_config();
  config.endpoints.realtime_url = "ws://127.0.0.1:38101/v1/events";
  config.endpoints.relay_url = "http://127.0.0.1:38102";
  return config;
}

net::NetworkClientIdentity identity() {
  net::NetworkClientIdentity value{};
  value.xenon_version = "test-build";
  value.host_platform = "test";
  value.host_architecture = "test-arch";
  value.requested_capabilities = {"profile", "sessions"};
  return value;
}

std::shared_ptr<net::XenonNetworkClient> client(
    const std::shared_ptr<net::NetworkTransport>& transport,
    net::NetworkConfig config = development_config()) {
  return std::make_shared<net::XenonNetworkClient>(transport, std::move(config), identity());
}

void test_offline_and_disabled_are_deterministic() {
  auto transport = std::make_shared<ScriptedTransport>();
  net::NetworkConfig disabled{};
  auto disabled_client = client(transport, disabled);
  bool disabled_callback = false;
  disabled_client->start([&](net::NetworkResult result) {
    disabled_callback = true;
    assert(result.error.code == net::NetworkErrorCode::Disabled);
  });
  assert(disabled_callback);
  assert(disabled_client->status().connection == net::NetworkConnectionState::Disabled);
  assert(transport->requests.empty());

  auto offline = development_config();
  offline.environment = net::NetworkEnvironment::Offline;
  auto offline_client = client(transport, offline);
  offline_client->start();
  assert(offline_client->status().connection == net::NetworkConnectionState::Offline);
  assert(transport->requests.empty());
}

void test_security_policy_and_endpoint_generation() {
  auto config = development_config();
  assert(!net::XenonNetworkClient::validate_config(config));
  config.endpoints.base_url = "http://example.test";
  assert(net::XenonNetworkClient::validate_config(config).code ==
         net::NetworkErrorCode::InvalidConfiguration);
  config.environment = net::NetworkEnvironment::Production;
  config.endpoints.base_url.clear();
  assert(net::XenonNetworkClient::validate_config(config).code ==
         net::NetworkErrorCode::EndpointNotConfigured);
  config.endpoints.base_url = "https://network.example.test";
  assert(!net::XenonNetworkClient::validate_config(config));
  config.endpoints.base_url = "https://";
  assert(net::XenonNetworkClient::validate_config(config).code ==
         net::NetworkErrorCode::InvalidConfiguration);
  config = development_config();
  config.endpoints.base_url = "http://[::1]:38100";
  assert(!net::XenonNetworkClient::validate_config(config));
  config.endpoints.base_url = "http://[::::]:38100";
  assert(net::XenonNetworkClient::validate_config(config).code ==
         net::NetworkErrorCode::InvalidConfiguration);
  config.endpoints.base_url = "https://network.example.test:0";
  assert(net::XenonNetworkClient::validate_config(config).code ==
         net::NetworkErrorCode::InvalidConfiguration);
  config.endpoints.base_url = "https://user%40name:password@network.example.test";
  assert(net::XenonNetworkClient::validate_config(config).code ==
         net::NetworkErrorCode::InvalidConfiguration);
  config.endpoints.base_url = "https://network.example.test/%2e%2e/private";
  assert(net::XenonNetworkClient::validate_config(config).code ==
         net::NetworkErrorCode::InvalidConfiguration);
  config.endpoints.base_url = "https://network.example.test\\@evil.example";
  assert(net::XenonNetworkClient::validate_config(config).code ==
         net::NetworkErrorCode::InvalidConfiguration);
  config = development_config();
  config.retry.jitter_ratio = std::numeric_limits<double>::quiet_NaN();
  assert(net::XenonNetworkClient::validate_config(config).code ==
         net::NetworkErrorCode::InvalidConfiguration);
  config = development_config();
  config.endpoints.realtime_url = "ws://example.test/v1/events";
  assert(net::XenonNetworkClient::validate_config(config).code ==
         net::NetworkErrorCode::InvalidConfiguration);

  assert(net::route_path(net::NetworkRoute::SessionJoin, "session / one") ==
         "/v1/sessions/session%20%2F%20one/join");
  assert(net::route_path(net::NetworkRoute::SessionGet).empty());
  assert(net::join_endpoint("https://network.example.test/", "/v1/health") ==
         "https://network.example.test/v1/health");
  assert(net::route_definition(net::NetworkRoute::ConnectivityAllocationCreate)
             .supports_idempotency);
}

void test_bootstrap_parser_fixtures() {
  net::NetworkBootstrap parsed{};
  net::NetworkError error{};
  assert(net::XenonNetworkClient::parse_bootstrap(
      response(200, fixture("bootstrap-valid.json")), parsed, error));
  assert(parsed.protocol_version == 1);
  assert(parsed.capabilities.contains("profile"));
  assert(!parsed.capabilities.contains("future-optional-capability"));

  parsed = {};
  error = {};
  assert(!net::XenonNetworkClient::parse_bootstrap(
      response(200, fixture("bootstrap-missing-required.json")), parsed, error));
  assert(error.code == net::NetworkErrorCode::MalformedResponse);

  auto wrong_content_type = response(200, fixture("bootstrap-valid.json"));
  wrong_content_type.headers.emplace("Content-Type", "text/html");
  parsed = {};
  error = {};
  assert(!net::XenonNetworkClient::parse_bootstrap(wrong_content_type, parsed, error));
  assert(error.code == net::NetworkErrorCode::MalformedResponse);

  parsed = {};
  error = {};
  assert(!net::XenonNetworkClient::parse_bootstrap(
      response(200, fixture("bootstrap-malformed.json")), parsed, error));
  assert(error.code == net::NetworkErrorCode::MalformedResponse);

  auto required_unknown = response(
      200, R"({"protocolVersion":1,"minimumClientProtocol":1,"requiredCapabilities":["quantum-party"]})");
  parsed = {};
  error = {};
  assert(!net::XenonNetworkClient::parse_bootstrap(required_unknown, parsed, error));
  assert(error.code == net::NetworkErrorCode::UnsupportedCapability);

  auto fractional = response(
      200, R"({"protocolVersion":1.5,"minimumClientProtocol":1,"capabilities":[]})");
  parsed = {};
  error = {};
  assert(!net::XenonNetworkClient::parse_bootstrap(fractional, parsed, error));
  assert(error.code == net::NetworkErrorCode::MalformedResponse);

  auto invalid_utf8 = response(
      200, std::string("{\"protocolVersion\":1,\"minimumClientProtocol\":1,\"serviceVersion\":\"") +
               static_cast<char>(0xFF) + "\"}");
  parsed = {};
  error = {};
  assert(!net::XenonNetworkClient::parse_bootstrap(invalid_utf8, parsed, error));
  assert(error.code == net::NetworkErrorCode::MalformedResponse);

  for (const auto& malformed : {
           R"({"protocolVersion":1,"protocolVersion":1,"minimumClientProtocol":1})",
           R"({"protocolVersion":01,"minimumClientProtocol":1})",
           R"({"protocolVersion":1,"minimumClientProtocol":1,"maintenance":"false"})",
           R"({"protocolVersion":1,"minimumClientProtocol":1,"serviceVersion":"\q"})",
           R"({"protocolVersion":1,"minimumClientProtocol":1,"serviceVersion":"\uD800"})",
           R"({"protocolVersion":1,"minimumClientProtocol":1,"endpoints":{"realtime":7}})",
       }) {
    parsed = {};
    error = {};
    assert(!net::XenonNetworkClient::parse_bootstrap(response(200, malformed), parsed, error));
    assert(error.code == net::NetworkErrorCode::MalformedResponse);
  }

  std::string deeply_nested = R"({"protocolVersion":1,"minimumClientProtocol":1,"unknown":)";
  deeply_nested.append(65u, '[');
  deeply_nested += '0';
  deeply_nested.append(65u, ']');
  deeply_nested += '}';
  parsed = {};
  error = {};
  assert(!net::XenonNetworkClient::parse_bootstrap(response(200, deeply_nested), parsed, error));
  assert(error.code == net::NetworkErrorCode::MalformedResponse);
}

void test_bootstrap_success_and_protocol_rejection() {
  auto transport = std::make_shared<ScriptedTransport>();
  transport->results.push_back({response(200, fixture("bootstrap-valid.json")), {}});
  auto network = client(transport, development_config_with_service_origins());
  std::vector<net::NetworkConnectionState> states;
  network->set_state_callback(
      [&](const net::NetworkStatus& status) { states.push_back(status.connection); });
  bool completed = false;
  network->start([&](net::NetworkResult result) {
    completed = true;
    assert(result.ok());
  });
  assert(completed);
  assert(network->status().connection == net::NetworkConnectionState::Ready);
  assert(network->status().service_reachable);
  assert(network->status().protocol_compatible);
  assert(transport->requests.size() == 1);
  assert(transport->requests.front().route_name == "bootstrap");
  assert(transport->requests.front().request_id ==
         transport->requests.front().headers.at("X-Request-ID"));
  assert(states.front() == net::NetworkConnectionState::Resolving);

  auto untrusted_transport = std::make_shared<ScriptedTransport>();
  untrusted_transport->results.push_back(
      {response(200,
                R"({"protocolVersion":1,"minimumClientProtocol":1,"endpoints":{"realtime":"wss://attacker.example/v1/events"}})"),
       {}});
  auto production = development_config();
  production.environment = net::NetworkEnvironment::Production;
  production.endpoints.base_url = "https://network.example.test";
  auto untrusted_client = client(untrusted_transport, production);
  net::NetworkErrorCode untrusted_error = net::NetworkErrorCode::None;
  untrusted_client->start(
      [&](net::NetworkResult result) { untrusted_error = result.error.code; });
  assert(untrusted_error == net::NetworkErrorCode::TlsFailure);
  assert(untrusted_client->status().connection == net::NetworkConnectionState::Unavailable);

  auto future_transport = std::make_shared<ScriptedTransport>();
  future_transport->results.push_back({response(200, fixture("bootstrap-future.json")), {}});
  auto future_client = client(future_transport);
  net::NetworkErrorCode completion_error = net::NetworkErrorCode::None;
  future_client->start(
      [&](net::NetworkResult result) { completion_error = result.error.code; });
  assert(completion_error == net::NetworkErrorCode::ProtocolMismatch);
  assert(future_client->status().connection == net::NetworkConnectionState::Error);
}

void test_retry_success_and_exhaustion() {
  auto transport = std::make_shared<ScriptedTransport>();
  transport->results.push_back(
      {{}, {net::NetworkErrorCode::ConnectionFailure, "refused"}});
  transport->results.push_back({response(200, fixture("bootstrap-valid.json")), {}});
  auto network = client(transport, development_config_with_service_origins());
  bool succeeded = false;
  network->start([&](net::NetworkResult result) { succeeded = result.ok(); });
  assert(succeeded);
  assert(transport->requests.size() == 2);
  assert(network->metrics().retries == 1);

  auto failing_transport = std::make_shared<ScriptedTransport>();
  for (int i = 0; i < 3; ++i) {
    failing_transport->results.push_back(
        {{}, {net::NetworkErrorCode::ServiceUnavailable, "unavailable"}});
  }
  auto failing = client(failing_transport);
  net::NetworkErrorCode final_error = net::NetworkErrorCode::None;
  failing->start([&](net::NetworkResult result) { final_error = result.error.code; });
  assert(final_error == net::NetworkErrorCode::ServiceUnavailable);
  assert(failing_transport->requests.size() == 3);
  assert(failing->metrics().retries == 2);
  assert(failing->metrics().requests_failed == 1);
}

void test_authentication_idempotency_and_handshake() {
  auto transport = std::make_shared<ScriptedTransport>();
  auto network = client(transport);
  bool auth_failed = false;
  network->perform({net::NetworkRoute::ProfileMe, {}, {}, {}, true},
                   [&](net::NetworkResult result) {
                     auth_failed =
                         result.error.code == net::NetworkErrorCode::AuthenticationRequired;
                   });
  assert(auth_failed && transport->requests.empty());

  auth_failed = false;
  network->perform({net::NetworkRoute::ProfileMe}, [&](net::NetworkResult result) {
    auth_failed = result.error.code == net::NetworkErrorCode::AuthenticationRequired;
  });
  assert(auth_failed && transport->requests.empty());

  assert(!network->set_auth_session({"bad\r\ntoken", {}, {}}));
  assert(network->status().authentication == net::NetworkAuthState::Error);

  assert(network->set_auth_session({"top-secret-access", "top-secret-refresh", "later"}));
  transport->results.push_back({response(200, "{}"), {}});
  network->perform({net::NetworkRoute::ClientHandshake, {}, {}, "handshake-key", true},
                   [](net::NetworkResult result) { assert(result.ok()); });
  assert(transport->requests.size() == 1);
  const auto& request = transport->requests.front();
  assert(request.headers.at("Authorization") == "Bearer top-secret-access");
  assert(request.headers.at("Idempotency-Key") == "handshake-key");
  assert(request.body.find("test-build") != std::string::npos);
  assert(request.body.find("protocolVersion") != std::string::npos);
  assert(net::sanitize_for_log(request.headers.at("Authorization")) == "[redacted]");
  assert(net::sanitize_for_log("access_token=top-secret-access").find("top-secret") ==
         std::string::npos);
  assert(net::sanitize_for_log("safe\r\nforged-line") == "safe??forged-line");

  transport->results.push_back({response(401, "{}"), {}});
  network->perform({net::NetworkRoute::ProfileMe}, [](net::NetworkResult result) {
    assert(result.error.code == net::NetworkErrorCode::AuthenticationRequired);
  });
  assert(network->status().authentication == net::NetworkAuthState::Expired);
  bool expired_token_rejected = false;
  network->perform({net::NetworkRoute::ProfileMe}, [&](net::NetworkResult result) {
    expired_token_rejected =
        result.error.code == net::NetworkErrorCode::AuthenticationRequired;
  });
  assert(expired_token_rejected);

  bool invalid_operation_rejected = false;
  network->perform({static_cast<net::NetworkRoute>(255)}, [&](net::NetworkResult result) {
    invalid_operation_rejected =
        result.error.code == net::NetworkErrorCode::InvalidConfiguration;
  });
  assert(invalid_operation_rejected);

  auto config = development_config();
  config.maximum_request_bytes = 8;
  auto limited = client(std::make_shared<ScriptedTransport>(), config);
  limited->perform({net::NetworkRoute::AuthSessionCreate, {}, "0123456789"},
                   [&](net::NetworkResult result) {
                     assert(result.error.code == net::NetworkErrorCode::RequestTooLarge);
                   });

  network->perform({net::NetworkRoute::Health, {}, "body"}, [](net::NetworkResult result) {
    assert(result.error.code == net::NetworkErrorCode::InvalidConfiguration);
  });
  network->perform({net::NetworkRoute::ClientHandshake, {}, {}, "bad key"},
                   [](net::NetworkResult result) {
                     assert(result.error.code == net::NetworkErrorCode::InvalidConfiguration);
                   });
}

void test_timeout_size_limit_cancellation_and_shutdown() {
  auto timeout_transport = std::make_shared<ScriptedTransport>();
  auto config = development_config();
  config.retry.maximum_retries = 0;
  timeout_transport->results.push_back({{}, {net::NetworkErrorCode::Timeout, "timeout"}});
  auto timeout_client = client(timeout_transport, config);
  timeout_client->check_health([](net::NetworkResult result) {
    assert(result.error.code == net::NetworkErrorCode::Timeout);
  });
  assert(timeout_client->metrics().timeouts == 1);

  auto large_transport = std::make_shared<ScriptedTransport>();
  config.maximum_response_bytes = 8;
  large_transport->results.push_back({response(200, "0123456789"), {}});
  auto large_client = client(large_transport, config);
  large_client->check_health([](net::NetworkResult result) {
    assert(result.error.code == net::NetworkErrorCode::ResponseTooLarge);
  });

  auto holding_transport = std::make_shared<HoldingTransport>();
  auto holding = client(holding_transport);
  bool cancelled = false;
  holding->check_health([&](net::NetworkResult result) {
    cancelled = result.error.code == net::NetworkErrorCode::Cancelled;
  });
  holding->shutdown();
  assert(holding_transport->shutdown_called);
  holding_transport->completion_({response(200, "{}"), {}});
  assert(cancelled);

  auto queue_transport = std::make_shared<HoldingTransport>();
  config = development_config();
  config.maximum_pending_requests = 1;
  auto queue_client = client(queue_transport, config);
  queue_client->check_health([](net::NetworkResult) {});
  bool queue_rejected = false;
  queue_client->check_health([&](net::NetworkResult result) {
    queue_rejected = result.error.code == net::NetworkErrorCode::ServiceUnavailable;
  });
  assert(queue_rejected);
  queue_client->shutdown();
}

void test_capability_report_shape() {
  net::NetworkStatus status{};
  status.connection = net::NetworkConnectionState::ConnectedTransport;
  status.transport_available = true;
  status.configured = true;
  status.service_reachable = true;
  status.protocol_compatible = true;
  status.capabilities.negotiated = {"sessions"};
  net::NetworkMetrics metrics{};
  metrics.requests_completed = 1;
  const auto json = net::network_status_json(status, metrics);
  assert(json.get_bool("clientCompiled"));
  assert(!json.get_bool("onlineServicesReady"));
  assert(json.get_string("connectionState") == "connected_transport");
  assert(json.find("metrics")->get_number("requestsCompleted") == 1.0);
}

void test_realtime_reconnect_and_clean_stop() {
  auto scheduler = std::make_shared<ScriptedTransport>();
  auto channel = std::make_shared<ScriptedRealtimeChannel>();
  channel->states = {net::NetworkRealtimeState::Error,
                     net::NetworkRealtimeState::Connected};
  net::RetryPolicy retry{};
  retry.initial_backoff = std::chrono::milliseconds(0);
  retry.maximum_backoff = std::chrono::milliseconds(0);
  auto realtime =
      std::make_shared<net::NetworkRealtimeController>(scheduler, channel, retry);
  realtime->start("ws://127.0.0.1/v1/events", "short-lived-secret",
                  [](net::NetworkRealtimeState, net::NetworkError) {},
                  [](net::NetworkRealtimeEvent) {});
  assert(channel->connect_count == 2);
  assert(realtime->state() == net::NetworkRealtimeState::Connected);
  assert(realtime->reconnect_count() == 1);
  int delivered = 0;
  channel->emit({"event-1", "session.joined", "{}"});
  // The callback supplied above intentionally does nothing; delivery itself
  // is covered by the bounded controller below.
  realtime->stop();
  assert(channel->disconnected);
  assert(realtime->state() == net::NetworkRealtimeState::Disconnected);
  channel->emit({"late-event", "session.joined", "{}"});

  auto bounded_channel = std::make_shared<ScriptedRealtimeChannel>();
  bounded_channel->states = {net::NetworkRealtimeState::Connected};
  auto bounded = std::make_shared<net::NetworkRealtimeController>(
      scheduler, bounded_channel, net::RetryPolicy{}, 8u);
  bounded->start("wss://network.example.test/v1/events", "short-lived-secret",
                 [](net::NetworkRealtimeState, net::NetworkError) {},
                 [&](net::NetworkRealtimeEvent) { ++delivered; });
  bounded_channel->emit({"event-1", "presence.changed", "{}"});
  assert(delivered == 1);
  bounded_channel->emit({"event-2", "presence.changed", "012345678"});
  assert(delivered == 1);
  assert(bounded->state() == net::NetworkRealtimeState::Unavailable);
  assert(bounded_channel->disconnected);

  auto restart_channel = std::make_shared<ScriptedRealtimeChannel>();
  restart_channel->states = {net::NetworkRealtimeState::Connected,
                             net::NetworkRealtimeState::Connected};
  auto restarted = std::make_shared<net::NetworkRealtimeController>(
      scheduler, restart_channel, net::RetryPolicy{});
  int restart_delivered = 0;
  restarted->start("wss://network.example.test/v1/events", "first-secret",
                   [](net::NetworkRealtimeState, net::NetworkError) {},
                   [&](net::NetworkRealtimeEvent) { ++restart_delivered; });
  const auto stale_event_callback = restart_channel->event_callback_;
  restarted->start("wss://network.example.test/v1/events", "second-secret",
                   [](net::NetworkRealtimeState, net::NetworkError) {},
                   [&](net::NetworkRealtimeEvent) { ++restart_delivered; });
  stale_event_callback({"stale-event", "presence.changed", "{}"});
  assert(restart_delivered == 0);
  restart_channel->emit({"current-event", "presence.changed", "{}"});
  assert(restart_delivered == 1);
}

void test_xbox_services_boundary_returns_truthful_offline_error() {
  auto transport = std::make_shared<ScriptedTransport>();
  auto network = client(transport, net::NetworkConfig{});
  network->start();
  net::XboxServicesNetworkAdapter adapter(network);
  const auto error = adapter.availability("friends", true);
  assert(error.code == net::NetworkErrorCode::Disabled);
  bool completed = false;
  adapter.perform("friends", true, {net::NetworkRoute::FriendsList},
                  [&](net::NetworkResult result) {
                    completed = true;
                    assert(result.error.code == net::NetworkErrorCode::Disabled);
                  });
  assert(completed);
  assert(transport->requests.empty());
}

}  // namespace

int main() {
  std::cout << "Testing Xenon Network client/uplink...\n";
  test_offline_and_disabled_are_deterministic();
  test_security_policy_and_endpoint_generation();
  test_bootstrap_parser_fixtures();
  test_bootstrap_success_and_protocol_rejection();
  test_retry_success_and_exhaustion();
  test_authentication_idempotency_and_handshake();
  test_timeout_size_limit_cancellation_and_shutdown();
  test_capability_report_shape();
  test_realtime_reconnect_and_clean_stop();
  test_xbox_services_boundary_returns_truthful_offline_error();
  std::cout << "All Xenon Network tests passed!\n";
  return 0;
}
