#include "xenon/network/client.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <limits>
#include <sstream>
#include <utility>

#include "xenon/logging/logger.hpp"

namespace xenon::network {
namespace {

std::atomic<std::uint64_t> g_request_counter{1};

std::string request_id() {
  const auto sequence = g_request_counter.fetch_add(1, std::memory_order_relaxed);
  const auto ticks = static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  std::ostringstream value;
  value << std::hex << ticks << '-' << sequence;
  return value.str();
}

std::string contact_timestamp() {
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch());
  return std::to_string(seconds.count());
}

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool is_loopback_host(std::string_view url) {
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) return false;
  const auto host_start = scheme_end + 3u;
  const auto host_end = url.find_first_of("/:?#", host_start);
  const auto authority = lowercase(std::string(url.substr(host_start, host_end - host_start)));
  return authority == "localhost" || authority == "127.0.0.1" || authority == "[::1]";
}

bool has_scheme(std::string_view url, std::string_view scheme) {
  if (url.size() < scheme.size()) return false;
  return lowercase(std::string(url.substr(0, scheme.size()))) == scheme;
}

bool secure_discovered_endpoint(std::string_view url, NetworkEnvironment environment,
                                bool realtime) {
  if (url.empty()) return true;
  const bool secure = realtime ? has_scheme(url, "wss://") : has_scheme(url, "https://");
  if (secure) return true;
  const bool local_plaintext = realtime ? has_scheme(url, "ws://") : has_scheme(url, "http://");
  return environment == NetworkEnvironment::Development && local_plaintext &&
         is_loopback_host(url);
}

NetworkError http_error(const NetworkResponse& response) {
  NetworkError error{};
  error.http_status = response.status_code;
  if (response.status_code >= 200 && response.status_code < 300) return error;
  switch (response.status_code) {
    case 401: error.code = NetworkErrorCode::AuthenticationRequired; break;
    case 403: error.code = NetworkErrorCode::Forbidden; break;
    case 404: error.code = NetworkErrorCode::NotFound; break;
    case 409: error.code = NetworkErrorCode::Conflict; break;
    case 429: error.code = NetworkErrorCode::RateLimited; break;
    case 501: error.code = NetworkErrorCode::NotImplemented; break;
    case 502:
    case 503:
    case 504: error.code = NetworkErrorCode::ServiceUnavailable; break;
    default:
      error.code = response.status_code >= 500 ? NetworkErrorCode::ServerError
                                               : NetworkErrorCode::MalformedResponse;
      break;
  }
  error.diagnostic = "Xenon Network returned HTTP " + std::to_string(response.status_code);
  return error;
}

bool parse_string_array(const core::JsonValue* value, std::vector<std::string>& output,
                        NetworkError& error) {
  if (value == nullptr) return true;
  const auto* array = value->as_array();
  if (array == nullptr || array->size() > kMaximumCapabilities) {
    error = {NetworkErrorCode::MalformedResponse, "Invalid capability list"};
    return false;
  }
  for (const auto& item : *array) {
    if (!item.is_string()) {
      error = {NetworkErrorCode::MalformedResponse, "Capability names must be strings"};
      return false;
    }
    auto text = item.as_string();
    if (text.empty() || text.size() > kMaximumIdentifierBytes) {
      error = {NetworkErrorCode::MalformedResponse, "Capability name exceeds protocol bounds"};
      return false;
    }
    output.push_back(std::move(text));
  }
  return true;
}

bool known_capability(std::string_view capability) {
  constexpr std::array<std::string_view, 8> kKnown{{
      "authentication", "profile", "presence", "friends", "matchmaking", "sessions",
      "connectivity", "realtime",
  }};
  return std::find(kKnown.begin(), kKnown.end(), capability) != kKnown.end();
}

}  // namespace

struct XenonNetworkClient::PendingRequest {
  NetworkOperation operation{};
  Completion completion{};
  std::uint32_t retry_count{0};
  bool bootstrap_request{false};
  std::chrono::steady_clock::time_point started_at{std::chrono::steady_clock::now()};
};

XenonNetworkClient::XenonNetworkClient(std::shared_ptr<NetworkTransport> transport,
                                       NetworkConfig config, NetworkClientIdentity identity,
                                       std::shared_ptr<CredentialStore> credentials)
    : transport_(std::move(transport)),
      config_(std::move(config)),
      identity_(std::move(identity)),
      credentials_(std::move(credentials)) {
  status_.transport_available = transport_ && transport_->available();
  status_.configured = !config_.endpoints.base_url.empty();
  if (credentials_ && credentials_->load()) {
    status_.authentication = NetworkAuthState::Authenticated;
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
  if (config.connect_timeout.count() <= 0 || config.request_timeout.count() <= 0 ||
      config.realtime_heartbeat_timeout.count() <= 0 || config.maximum_response_bytes == 0 ||
      config.maximum_response_bytes > 16u * 1024u * 1024u ||
      config.maximum_pending_requests == 0 || config.maximum_queued_events == 0) {
    return {NetworkErrorCode::InvalidConfiguration, "Network limits and timeouts must be bounded"};
  }
  const auto& url = config.endpoints.base_url;
  if (url.find('@') != std::string::npos || url.find('#') != std::string::npos) {
    return {NetworkErrorCode::InvalidConfiguration,
            "Endpoint URLs may not contain credentials or fragments"};
  }
  if (has_scheme(url, "https://")) return {};
  if (config.environment == NetworkEnvironment::Development && has_scheme(url, "http://") &&
      is_loopback_host(url)) {
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
  {
    std::scoped_lock lock(mutex_);
    if (shutdown_) {
      if (completion) {
        completion(NetworkResult{{}, {NetworkErrorCode::Cancelled, "Network client is shut down"}});
      }
      return;
    }
  }
  if (operation.authenticated && access_token().empty()) {
    NetworkError error{NetworkErrorCode::AuthenticationRequired,
                       "This Xenon Network operation requires authentication"};
    if (completion) completion(NetworkResult{{}, std::move(error)});
    return;
  }
  PendingRequest pending{};
  pending.operation = std::move(operation);
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
  if (operation.authenticated) {
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

  {
    std::scoped_lock lock(mutex_);
    metrics_.bytes_sent += result.response.bytes_sent;
    metrics_.bytes_received += result.response.bytes_received;
    if (result.error) {
      ++metrics_.requests_failed;
      if (result.error.code == NetworkErrorCode::Timeout) ++metrics_.timeouts;
      status_.last_error = result.error;
      if (pending.operation.route == NetworkRoute::Health) status_.service_reachable = false;
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

bool XenonNetworkClient::parse_bootstrap(const NetworkResponse& response,
                                         NetworkBootstrap& bootstrap, NetworkError& error) {
  core::JsonValue root;
  std::string parse_error;
  if (!core::JsonValue::parse(response.body, root, &parse_error) || !root.is_object()) {
    error = {NetworkErrorCode::MalformedResponse,
             "Bootstrap response is not valid JSON: " + sanitize_for_log(parse_error)};
    return false;
  }
  const auto* protocol = root.find("protocolVersion");
  const auto* minimum = root.find("minimumClientProtocol");
  if (protocol == nullptr || minimum == nullptr ||
      protocol->type() != core::JsonValue::Type::Number ||
      minimum->type() != core::JsonValue::Type::Number) {
    error = {NetworkErrorCode::MalformedResponse,
             "Bootstrap response is missing required protocol version fields"};
    return false;
  }
  const auto protocol_number = protocol->as_number(-1.0);
  const auto minimum_number = minimum->as_number(-1.0);
  if (protocol_number < 0.0 || minimum_number < 0.0 ||
      protocol_number > std::numeric_limits<std::uint32_t>::max() ||
      minimum_number > std::numeric_limits<std::uint32_t>::max()) {
    error = {NetworkErrorCode::MalformedResponse, "Bootstrap protocol versions are invalid"};
    return false;
  }
  bootstrap.protocol_version = static_cast<std::uint32_t>(protocol_number);
  bootstrap.minimum_client_protocol = static_cast<std::uint32_t>(minimum_number);
  bootstrap.service_version = root.get_string("serviceVersion");
  bootstrap.server_time = root.get_string("serverTime");
  bootstrap.maintenance = root.get_bool("maintenance", false);
  bootstrap.authentication_required = root.get_bool("authenticationRequired", false);

  std::vector<std::string> advertised;
  if (!parse_string_array(root.find("capabilities"), advertised, error) ||
      !parse_string_array(root.find("requiredCapabilities"), bootstrap.required_capabilities,
                          error)) {
    return false;
  }
  for (const auto& capability : bootstrap.required_capabilities) {
    if (!known_capability(capability)) {
      error = {NetworkErrorCode::UnsupportedCapability,
               "Server requires unsupported capability: " + capability};
      return false;
    }
  }
  for (const auto& capability : advertised) {
    if (known_capability(capability)) bootstrap.capabilities.negotiated.push_back(capability);
  }

  if (const auto* endpoints = root.find("endpoints"); endpoints != nullptr) {
    if (!endpoints->is_object()) {
      error = {NetworkErrorCode::MalformedResponse, "Bootstrap endpoints must be an object"};
      return false;
    }
    bootstrap.discovered_endpoints.realtime_url = endpoints->get_string("realtime");
    bootstrap.discovered_endpoints.relay_url = endpoints->get_string("relay");
  }
  return true;
}

bool XenonNetworkClient::apply_bootstrap(const NetworkResponse& response, NetworkError& error) {
  NetworkBootstrap parsed{};
  if (!parse_bootstrap(response, parsed, error)) return false;
  if (parsed.protocol_version < kMinimumClientProtocolVersion ||
      parsed.protocol_version > kClientProtocolVersion ||
      parsed.minimum_client_protocol > kClientProtocolVersion) {
    error = {NetworkErrorCode::ProtocolMismatch,
             "Xenon Network protocol is incompatible with this client"};
    return false;
  }
  if (!secure_discovered_endpoint(parsed.discovered_endpoints.realtime_url, config_.environment,
                                  true) ||
      !secure_discovered_endpoint(parsed.discovered_endpoints.relay_url, config_.environment,
                                  false)) {
    error = {NetworkErrorCode::TlsFailure,
             "Bootstrap returned an endpoint that violates the transport security policy"};
    return false;
  }
  {
    std::scoped_lock lock(mutex_);
    bootstrap_ = parsed;
    status_.protocol_compatible = true;
    status_.capabilities = parsed.capabilities;
  }
  return true;
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

void XenonNetworkClient::set_auth_session(AuthSession session) {
  if (!credentials_) return;
  credentials_->store(session);
  {
    std::scoped_lock lock(mutex_);
    status_.authentication = NetworkAuthState::Authenticated;
  }
}

void XenonNetworkClient::clear_auth_session(NetworkAuthState state) {
  if (credentials_) credentials_->clear();
  std::scoped_lock lock(mutex_);
  status_.authentication = state;
}

std::string XenonNetworkClient::access_token() const {
  if (!credentials_) return {};
  const auto session = credentials_->load();
  return session ? session->access_token : std::string{};
}

}  // namespace xenon::network
