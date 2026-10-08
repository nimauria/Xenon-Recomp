// XenonNetworkClient: configuration, connection state, request issue and
// result handling, auth session.

#include "network/client/client_internal.hpp"

namespace xenon::network {

using namespace client_detail;

XenonNetworkClient::XenonNetworkClient(std::shared_ptr<NetworkTransport> transport,
                                       NetworkConfig config, NetworkClientIdentity identity,
                                       std::shared_ptr<CredentialStore> credentials)
    : transport_(std::move(transport)),
      config_(std::move(config)),
      identity_(std::move(identity)),
      credentials_(std::move(credentials)) {
  status_.transport_available = transport_ && transport_->available();
  status_.configured = !config_.endpoints.base_url.empty();
  if (credentials_) {
    const auto session = credentials_->load();
    if (session && valid_auth_session(*session)) {
      status_.authentication = NetworkAuthState::Authenticated;
    }
  }
}

XenonNetworkClient::~XenonNetworkClient() { shutdown(); }

void XenonNetworkClient::set_state_callback(StateCallback callback) {
  std::scoped_lock lock(mutex_);
  state_callback_ = std::move(callback);
}

NetworkError XenonNetworkClient::validate_config(const NetworkConfig& config) {
  if (!config.enabled) return {NetworkErrorCode::Disabled, "Xenon Network is disabled"};
  if (config.environment == NetworkEnvironment::Offline) {
    return {NetworkErrorCode::Offline, "Xenon is configured for offline operation"};
  }
  if (config.endpoints.base_url.empty()) {
    return {NetworkErrorCode::EndpointNotConfigured, "No Xenon Network endpoint is configured"};
  }
  constexpr auto kMaximumConnectTimeout = std::chrono::seconds(60);
  constexpr auto kMaximumOperationTimeout = std::chrono::minutes(5);
  constexpr auto kMaximumBackoff = std::chrono::minutes(5);
  constexpr std::size_t kMaximumPayloadBytes = 16u * 1024u * 1024u;
  if (config.connect_timeout.count() <= 0 || config.connect_timeout > kMaximumConnectTimeout ||
      config.request_timeout.count() <= 0 || config.request_timeout > kMaximumOperationTimeout ||
      config.realtime_heartbeat_timeout.count() <= 0 ||
      config.realtime_heartbeat_timeout > kMaximumOperationTimeout ||
      config.maximum_request_bytes == 0 || config.maximum_request_bytes > kMaximumPayloadBytes ||
      config.maximum_response_bytes == 0 || config.maximum_response_bytes > kMaximumPayloadBytes ||
      config.maximum_pending_requests == 0 || config.maximum_pending_requests > 1024u ||
      config.maximum_queued_events == 0 || config.maximum_queued_events > 4096u) {
    return {NetworkErrorCode::InvalidConfiguration, "Network limits and timeouts must be bounded"};
  }
  const auto& url = config.endpoints.base_url;
  ParsedEndpoint endpoint{};
  if (!parse_endpoint(url, endpoint) || config.retry.maximum_retries > 10u ||
      config.retry.initial_backoff.count() < 0 || config.retry.maximum_backoff.count() < 0 ||
      config.retry.initial_backoff > config.retry.maximum_backoff ||
      config.retry.maximum_backoff > kMaximumBackoff || !std::isfinite(config.retry.jitter_ratio) ||
      config.retry.jitter_ratio < 0.0 || config.retry.jitter_ratio > 1.0 ||
      !secure_discovered_endpoint(config.endpoints.realtime_url, config.environment, true) ||
      !secure_discovered_endpoint(config.endpoints.relay_url, config.environment, false)) {
    return {NetworkErrorCode::InvalidConfiguration,
            "Endpoint URL or retry policy is invalid"};
  }
  if (endpoint.scheme == "https") return {};
  if (config.environment == NetworkEnvironment::Development && endpoint.scheme == "http" &&
      is_loopback_host(endpoint)) {
    return {};
  }
  return {NetworkErrorCode::InvalidConfiguration,
          "HTTPS is required; plaintext is allowed only for an explicit loopback development endpoint"};
}

void XenonNetworkClient::start(Completion completion) {
  const auto config_error = validate_config(config_);
  if (config_error) {
    const auto state = config_error.code == NetworkErrorCode::Disabled
                           ? NetworkConnectionState::Disabled
                       : config_error.code == NetworkErrorCode::Offline
                           ? NetworkConnectionState::Offline
                           : NetworkConnectionState::Unavailable;
    transition(state, config_error);
    if (completion) completion(NetworkResult{{}, config_error});
    return;
  }
  if (!transport_ || !transport_->available()) {
    NetworkError error{NetworkErrorCode::ServiceUnavailable,
                       "No Xenon Network transport implementation is available"};
    transition(NetworkConnectionState::Unavailable, error);
    if (completion) completion(NetworkResult{{}, std::move(error)});
    return;
  }
  NetworkError admission_error{};
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_ || pending_operations_ >= config_.maximum_pending_requests) {
      admission_error = {shutdown_ ? NetworkErrorCode::Cancelled
                                   : NetworkErrorCode::ServiceUnavailable,
                         shutdown_ ? "Network client is shut down"
                                   : "Network request queue is full"};
    } else {
      ++pending_operations_;
    }
  }
  if (admission_error) {
    if (completion) completion(NetworkResult{{}, std::move(admission_error)});
    return;
  }
  transition(NetworkConnectionState::Resolving);
  transition(NetworkConnectionState::Connecting);
  PendingRequest pending{};
  pending.operation.route = NetworkRoute::Bootstrap;
  pending.completion = std::move(completion);
  pending.bootstrap_request = true;
  issue(std::move(pending));
}

void XenonNetworkClient::check_health(Completion completion) {
  perform(NetworkOperation{NetworkRoute::Health}, std::move(completion));
}

void XenonNetworkClient::perform(NetworkOperation operation, Completion completion) {
  const auto config_error = validate_config(config_);
  if (config_error) {
    if (completion) completion(NetworkResult{{}, config_error});
    return;
  }
  const auto operation_error = validate_operation(operation, config_);
  if (operation_error) {
    if (completion) completion(NetworkResult{{}, operation_error});
    return;
  }
  const auto& definition = route_definition(operation.route);
  const bool requires_authentication = definition.authentication_required || operation.authenticated;
  NetworkError immediate_error{};
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_) {
      immediate_error = {NetworkErrorCode::Cancelled, "Network client is shut down"};
    } else if (pending_operations_ >= config_.maximum_pending_requests) {
      immediate_error = {NetworkErrorCode::ServiceUnavailable, "Network request queue is full"};
    } else {
      ++pending_operations_;
    }
  }
  if (immediate_error) {
    if (completion) completion(NetworkResult{{}, std::move(immediate_error)});
    return;
  }
  if (!transport_ || !transport_->available()) {
    {
      std::scoped_lock lock(mutex_);
      --pending_operations_;
    }
    if (completion) {
      completion(NetworkResult{{}, {NetworkErrorCode::ServiceUnavailable,
                                     "No Xenon Network transport is available"}});
    }
    return;
  }
  if (requires_authentication && access_token().empty()) {
    NetworkError error{NetworkErrorCode::AuthenticationRequired,
                       "This Xenon Network operation requires authentication"};
    {
      std::scoped_lock lock(mutex_);
      --pending_operations_;
    }
    if (completion) completion(NetworkResult{{}, std::move(error)});
    return;
  }
  PendingRequest pending{};
  pending.operation = std::move(operation);
  pending.operation.authenticated = requires_authentication;
  pending.completion = std::move(completion);
  pending.bootstrap_request = pending.operation.route == NetworkRoute::Bootstrap;
  issue(std::move(pending));
}

NetworkRequest XenonNetworkClient::make_request(const NetworkOperation& operation) const {
  const auto& definition = route_definition(operation.route);
  NetworkRequest request{};
  request.request_id = request_id();
  request.route_name = std::string(definition.name);
  request.method = definition.method;
  request.url = join_endpoint(config_.endpoints.base_url,
                              route_path(operation.route, operation.identifier));
  request.body = operation.body;
  request.idempotency_key = operation.idempotency_key;
  request.connect_timeout = config_.connect_timeout;
  request.request_timeout = config_.request_timeout;
  request.maximum_response_bytes = config_.maximum_response_bytes;
  request.headers.emplace("Accept", "application/json");
  request.headers.emplace("X-Xenon-Protocol", std::to_string(kClientProtocolVersion));
  request.headers.emplace("X-Request-ID", request.request_id);
  if (!request.body.empty()) request.headers.emplace("Content-Type", "application/json");
  if (!request.idempotency_key.empty()) {
    request.headers.emplace("Idempotency-Key", request.idempotency_key);
  }
  if (definition.authentication_required || operation.authenticated) {
    const auto token = access_token();
    if (!token.empty()) request.headers.emplace("Authorization", "Bearer " + token);
  }
  if (operation.route == NetworkRoute::ClientHandshake && request.body.empty()) {
    request.body = identity_.handshake_json(request.request_id).dump();
    request.headers.emplace("Content-Type", "application/json");
  }
  return request;
}

void XenonNetworkClient::issue(PendingRequest pending) {
  if (cancellation_.token().cancelled()) {
    handle_result(std::move(pending),
                  NetworkResult{{}, {NetworkErrorCode::Cancelled,
                                     "Network operation was cancelled"}});
    return;
  }
  const auto request = make_request(pending.operation);
  if (request.url.empty()) {
    NetworkResult result{{}, {NetworkErrorCode::InvalidConfiguration,
                              "The network route identifier is missing or invalid"}};
    handle_result(std::move(pending), std::move(result));
    return;
  }
  {
    std::scoped_lock lock(mutex_);
    ++metrics_.requests_attempted;
  }
  xenon::logging::Logger::instance().log_if_enabled(
      xenon::logging::Level::Debug, "network", [&] {
        return "request=" + request.request_id + " route=" + request.route_name +
               " retry=" + std::to_string(pending.retry_count);
      });
  const auto weak = weak_from_this();
  transport_->send(request, cancellation_.token(),
                   [weak, pending = std::move(pending)](NetworkResult result) mutable {
                     if (const auto self = weak.lock()) {
                       self->handle_result(std::move(pending), std::move(result));
                     }
                   });
}

void XenonNetworkClient::handle_result(PendingRequest pending, NetworkResult result) {
  if (cancellation_.token().cancelled()) {
    result.error = {NetworkErrorCode::Cancelled, "Network operation was cancelled"};
  }
  if (!result.error) result.error = http_error(result.response);
  if (!result.error && result.response.body.size() > config_.maximum_response_bytes) {
    result.error = {NetworkErrorCode::ResponseTooLarge,
                    "Network response exceeded the configured size limit"};
    result.response.body.clear();
  }

  const auto& definition = route_definition(pending.operation.route);
  const bool safe_to_retry = definition.method == NetworkHttpMethod::Get ||
                             (!pending.operation.idempotency_key.empty() &&
                              definition.supports_idempotency);
  if (result.error.retryable() && safe_to_retry &&
      pending.retry_count < config_.retry.maximum_retries &&
      !cancellation_.token().cancelled()) {
    const auto delay = result.error.retry_after.count() > 0
                           ? result.error.retry_after
                           : config_.retry.delay_for(pending.retry_count,
                                                     g_request_counter.load(std::memory_order_relaxed));
    ++pending.retry_count;
    {
      std::scoped_lock lock(mutex_);
      ++metrics_.retries;
      if (pending.bootstrap_request) ++metrics_.reconnects;
    }
    if (pending.bootstrap_request) transition(NetworkConnectionState::Reconnecting, result.error);
    const auto weak = weak_from_this();
    transport_->schedule(delay, cancellation_.token(),
                         [weak, pending = std::move(pending)]() mutable {
                           if (const auto self = weak.lock()) self->issue(std::move(pending));
                         });
    return;
  }

  if (!result.error && pending.bootstrap_request) {
    transition(NetworkConnectionState::ConnectedTransport);
    NetworkError parse_error{};
    if (!apply_bootstrap(result.response, parse_error)) result.error = std::move(parse_error);
  }

  const bool authentication_expired =
      pending.operation.authenticated &&
      (result.error.code == NetworkErrorCode::AuthenticationRequired ||
       result.error.code == NetworkErrorCode::AuthenticationExpired);
  if (authentication_expired && credentials_) credentials_->clear();

  {
    std::scoped_lock lock(mutex_);
    metrics_.bytes_sent += result.response.bytes_sent;
    metrics_.bytes_received += result.response.bytes_received;
    if (pending_operations_ > 0) --pending_operations_;
    if (result.error) {
      ++metrics_.requests_failed;
      if (result.error.code == NetworkErrorCode::Timeout) ++metrics_.timeouts;
      if (authentication_expired) {
        status_.authentication = NetworkAuthState::Expired;
      }
      status_.last_error = result.error;
      status_.service_reachable = result.response.status_code > 0;
    } else {
      ++metrics_.requests_completed;
      status_.service_reachable = true;
      status_.last_successful_contact = contact_timestamp();
      status_.last_error = {};
    }
  }

  if (pending.bootstrap_request) {
    if (result.error) {
      transition(result.error.code == NetworkErrorCode::ProtocolMismatch ||
                         result.error.code == NetworkErrorCode::UnsupportedCapability
                     ? NetworkConnectionState::Error
                     : NetworkConnectionState::Unavailable,
                 result.error);
    } else {
      const auto current = status();
      transition((bootstrap()->maintenance ||
                  (bootstrap()->authentication_required &&
                   current.authentication != NetworkAuthState::Authenticated))
                     ? NetworkConnectionState::Degraded
                     : NetworkConnectionState::Ready);
    }
  }
  if (pending.completion) pending.completion(std::move(result));
}

void XenonNetworkClient::transition(NetworkConnectionState state, NetworkError error) {
  StateCallback callback;
  NetworkStatus snapshot;
  NetworkConnectionState previous;
  {
    std::scoped_lock lock(mutex_);
    previous = status_.connection;
    status_.connection = state;
    if (error) status_.last_error = std::move(error);
    snapshot = status_;
    callback = state_callback_;
  }
  xenon::logging::Logger::instance().log_if_enabled(
      xenon::logging::Level::Info, "network", [&] {
        return "state=" + std::string(to_string(previous)) + "->" +
               std::string(to_string(state)) + " error=" +
               std::string(to_string(snapshot.last_error.code));
      });
  if (callback) callback(snapshot);
}

void XenonNetworkClient::shutdown() {
  std::shared_ptr<NetworkTransport> transport;
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_) return;
    shutdown_ = true;
    transport = transport_;
  }
  cancellation_.cancel();
  if (transport) transport->shutdown();
  if (credentials_ && !credentials_->persistent()) credentials_->clear();
}

NetworkStatus XenonNetworkClient::status() const {
  std::scoped_lock lock(mutex_);
  return status_;
}

NetworkMetrics XenonNetworkClient::metrics() const {
  std::scoped_lock lock(mutex_);
  return metrics_;
}

std::optional<NetworkBootstrap> XenonNetworkClient::bootstrap() const {
  std::scoped_lock lock(mutex_);
  return bootstrap_;
}

NetworkConfig XenonNetworkClient::config() const {
  std::scoped_lock lock(mutex_);
  return config_;
}

bool XenonNetworkClient::set_auth_session(AuthSession session) {
  if (!credentials_ || !valid_auth_session(session) || !credentials_->store(session)) {
    secure_erase(session.access_token);
    secure_erase(session.refresh_token);
    StateCallback callback;
    NetworkStatus snapshot;
    {
      std::scoped_lock lock(mutex_);
      status_.authentication = NetworkAuthState::Error;
      status_.last_error = {NetworkErrorCode::InvalidConfiguration,
                            "Authentication session was rejected"};
      callback = state_callback_;
      snapshot = status_;
    }
    if (callback) callback(snapshot);
    return false;
  }
  secure_erase(session.access_token);
  secure_erase(session.refresh_token);
  {
    std::scoped_lock lock(mutex_);
    status_.authentication = NetworkAuthState::Authenticated;
    status_.last_error = {};
  }
  return true;
}

void XenonNetworkClient::clear_auth_session(NetworkAuthState state) {
  if (credentials_) credentials_->clear();
  std::scoped_lock lock(mutex_);
  status_.authentication = state;
}

std::string XenonNetworkClient::access_token() const {
  if (!credentials_) return {};
  const auto session = credentials_->load();
  return session && valid_auth_session(*session) ? session->access_token : std::string{};
}

}  // namespace xenon::network
