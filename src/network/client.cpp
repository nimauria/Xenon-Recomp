#include "xenon/network/client.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
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

struct ParsedEndpoint {
  std::string scheme{};
  std::string host{};
  std::uint16_t port{0};
};

int hex_value(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool parse_port(std::string_view port, std::uint16_t& parsed) {
  if (port.empty() || port.size() > 5u) return false;
  std::uint32_t value = 0;
  for (const unsigned char c : port) {
    if (!std::isdigit(c)) return false;
    value = value * 10u + static_cast<std::uint32_t>(c - '0');
  }
  if (value == 0u || value > 65535u) return false;
  parsed = static_cast<std::uint16_t>(value);
  return true;
}

bool valid_dns_host(std::string_view host) {
  if (host.empty() || host.size() > 253u || host.front() == '.' || host.back() == '.') return false;
  std::size_t label_start = 0;
  while (label_start < host.size()) {
    const auto label_end = host.find('.', label_start);
    const auto length = (label_end == std::string_view::npos ? host.size() : label_end) - label_start;
    if (length == 0u || length > 63u || host[label_start] == '-' ||
        host[label_start + length - 1u] == '-') {
      return false;
    }
    for (std::size_t index = label_start; index < label_start + length; ++index) {
      const auto c = static_cast<unsigned char>(host[index]);
      if (!std::isalnum(c) && c != '-') return false;
    }
    if (label_end == std::string_view::npos) break;
    label_start = label_end + 1u;
  }
  return true;
}

bool valid_ipv6_literal(std::string_view host) {
  if (host.empty() || host.find('.') != std::string_view::npos ||
      host.find(":::") != std::string_view::npos) {
    return false;
  }
  if ((host.front() == ':' && !host.starts_with("::")) ||
      (host.back() == ':' && !host.ends_with("::"))) {
    return false;
  }
  const auto compression = host.find("::");
  if (compression != std::string_view::npos && host.find("::", compression + 2u) != std::string_view::npos) {
    return false;
  }
  std::size_t groups = 0;
  std::size_t start = 0;
  while (start <= host.size()) {
    const auto end = host.find(':', start);
    const auto length = (end == std::string_view::npos ? host.size() : end) - start;
    if (length != 0u) {
      if (length > 4u) return false;
      for (std::size_t index = start; index < start + length; ++index) {
        if (!std::isxdigit(static_cast<unsigned char>(host[index]))) return false;
      }
      ++groups;
    }
    if (end == std::string_view::npos) break;
    start = end + 1u;
  }
  return compression == std::string_view::npos ? groups == 8u : groups < 8u;
}

bool valid_url_path(std::string_view path) {
  if (path.empty()) return true;
  if (path.front() != '/' || path.starts_with("//") || path.find('\\') != std::string_view::npos) {
    return false;
  }
  std::string segment;
  for (std::size_t index = 1; index <= path.size(); ++index) {
    if (index == path.size() || path[index] == '/') {
      if (segment == "." || segment == "..") return false;
      segment.clear();
      continue;
    }
    auto c = static_cast<unsigned char>(path[index]);
    if (c < 0x21u || c > 0x7Eu || c == '?' || c == '#') return false;
    if (c == '%') {
      if (index + 2u >= path.size()) return false;
      const int high = hex_value(path[index + 1u]);
      const int low = hex_value(path[index + 2u]);
      if (high < 0 || low < 0) return false;
      c = static_cast<unsigned char>((high << 4) | low);
      if (c < 0x21u || c == 0x7Fu || c == '/' || c == '\\' || c == '?' || c == '#' ||
          c == '@') {
        return false;
      }
      index += 2u;
    }
    segment.push_back(static_cast<char>(std::tolower(c)));
  }
  return true;
}

bool parse_endpoint(std::string_view url, ParsedEndpoint& parsed) {
  if (url.empty() || url.size() > 2048u || url.find_first_of("?#@\\") != std::string_view::npos) {
    return false;
  }
  for (const unsigned char c : url) {
    if (c < 0x21u || c > 0x7Eu) return false;
  }
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos || scheme_end == 0u) return false;
  parsed.scheme = lowercase(std::string(url.substr(0, scheme_end)));
  if (parsed.scheme != "https" && parsed.scheme != "http" && parsed.scheme != "wss" &&
      parsed.scheme != "ws") {
    return false;
  }
  const auto authority_start = scheme_end + 3u;
  const auto authority_end = url.find('/', authority_start);
  const auto authority = url.substr(
      authority_start, (authority_end == std::string_view::npos ? url.size() : authority_end) -
                           authority_start);
  if (authority.empty()) return false;

  std::string_view host;
  std::string_view port;
  bool port_present = false;
  if (authority.front() == '[') {
    const auto close = authority.find(']');
    if (close == std::string_view::npos || close == 1u) return false;
    host = authority.substr(1u, close - 1u);
    if (!valid_ipv6_literal(host)) return false;
    const auto suffix = authority.substr(close + 1u);
    if (!suffix.empty()) {
      if (suffix.front() != ':') return false;
      port_present = true;
      port = suffix.substr(1u);
    }
  } else {
    const auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
      if (authority.find(':') != colon) return false;
      host = authority.substr(0u, colon);
      port_present = true;
      port = authority.substr(colon + 1u);
    } else {
      host = authority;
    }
    if (!valid_dns_host(host)) return false;
  }
  parsed.port = (parsed.scheme == "https" || parsed.scheme == "wss") ? 443u : 80u;
  if (port_present && !parse_port(port, parsed.port)) return false;
  parsed.host = lowercase(std::string(host));
  return valid_url_path(authority_end == std::string_view::npos ? std::string_view{}
                                                               : url.substr(authority_end));
}

bool is_loopback_host(const ParsedEndpoint& endpoint) {
  return endpoint.host == "localhost" || endpoint.host == "127.0.0.1" ||
         endpoint.host == "::1";
}

bool secure_discovered_endpoint(std::string_view url, NetworkEnvironment environment,
                                bool realtime) {
  if (url.empty()) return true;
  ParsedEndpoint endpoint{};
  if (!parse_endpoint(url, endpoint)) return false;
  const bool secure = realtime ? endpoint.scheme == "wss" : endpoint.scheme == "https";
  if (secure) return true;
  const bool local_plaintext = realtime ? endpoint.scheme == "ws" : endpoint.scheme == "http";
  return environment == NetworkEnvironment::Development && local_plaintext &&
         is_loopback_host(endpoint);
}

bool authorized_discovered_endpoint(std::string_view discovered,
                                    std::string_view configured,
                                    std::string_view base_url,
                                    NetworkEnvironment environment,
                                    bool realtime) {
  if (!secure_discovered_endpoint(discovered, environment, realtime)) return false;
  if (discovered.empty()) return true;
  ParsedEndpoint discovered_endpoint{};
  ParsedEndpoint trusted_endpoint{};
  if (!parse_endpoint(discovered, discovered_endpoint) ||
      !parse_endpoint(configured.empty() ? base_url : configured, trusted_endpoint)) {
    return false;
  }
  return discovered_endpoint.host == trusted_endpoint.host &&
         discovered_endpoint.port == trusted_endpoint.port;
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
  for (const auto& [name, value] : response.headers) {
    if (lowercase(name) != "retry-after" || value.empty() || value.size() > 10u) continue;
    std::uint64_t seconds = 0;
    bool numeric = true;
    for (const unsigned char c : value) {
      if (!std::isdigit(c)) {
        numeric = false;
        break;
      }
      seconds = std::min<std::uint64_t>(seconds * 10u + static_cast<std::uint64_t>(c - '0'), 300u);
    }
    if (numeric) error.retry_after = std::chrono::seconds(seconds);
    break;
  }
  return error;
}

bool valid_protocol_identifier(std::string_view value) {
  if (value.empty() || value.size() > kMaximumIdentifierBytes) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == ':';
  });
}

bool valid_protocol_text(std::string_view value, std::size_t maximum_bytes) {
  if (value.size() > maximum_bytes || !valid_utf8(value)) return false;
  return std::none_of(value.begin(), value.end(), [](unsigned char c) {
    return c < 0x20u || c == 0x7Fu;
  });
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
    if (!valid_protocol_identifier(text) ||
        std::find(output.begin(), output.end(), text) != output.end()) {
      error = {NetworkErrorCode::MalformedResponse,
               "Capability name is invalid or duplicated"};
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

NetworkError validate_operation(const NetworkOperation& operation, const NetworkConfig& config) {
  if (!valid_network_route(operation.route)) {
    return {NetworkErrorCode::InvalidConfiguration, "Unknown Xenon Network route"};
  }
  const auto& definition = route_definition(operation.route);
  if (operation.body.size() > config.maximum_request_bytes) {
    return {NetworkErrorCode::RequestTooLarge,
            "Network request exceeded the configured size limit"};
  }
  if (definition.method == NetworkHttpMethod::Get && !operation.body.empty()) {
    return {NetworkErrorCode::InvalidConfiguration, "GET operations may not contain a body"};
  }
  if (definition.identifier_required != !operation.identifier.empty() ||
      (!operation.identifier.empty() && !valid_route_identifier(operation.identifier))) {
    return {NetworkErrorCode::InvalidConfiguration,
            "Network route identifier is missing, unexpected, or invalid"};
  }
  if (!operation.idempotency_key.empty() &&
      (!definition.supports_idempotency ||
       !valid_protocol_identifier(operation.idempotency_key))) {
    return {NetworkErrorCode::InvalidConfiguration, "Idempotency key is invalid for this route"};
  }
  return {};
}

bool valid_auth_session(const AuthSession& session) {
  const auto valid_credential = [](std::string_view value) {
    return !value.empty() && value.size() <= kMaximumCredentialBytes &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
             return c >= 0x21u && c <= 0x7Eu;
           });
  };
  return valid_credential(session.access_token) &&
         (session.refresh_token.empty() ||
          valid_credential(session.refresh_token)) &&
         valid_protocol_text(session.expires_at, 128u);
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

bool XenonNetworkClient::parse_bootstrap(const NetworkResponse& response,
                                         NetworkBootstrap& bootstrap, NetworkError& error) {
  if (response.body.size() > kDefaultMaximumResponseBytes || !valid_utf8(response.body)) {
    error = {response.body.size() > kDefaultMaximumResponseBytes
                 ? NetworkErrorCode::ResponseTooLarge
                 : NetworkErrorCode::MalformedResponse,
             "Bootstrap response violates protocol size or UTF-8 limits"};
    return false;
  }
  for (const auto& [name, value] : response.headers) {
    if (lowercase(name) != "content-type") continue;
    const auto content_type = lowercase(value);
    const auto semicolon = content_type.find(';');
    const auto media_type = content_type.substr(0u, semicolon);
    if (media_type != "application/json" && !media_type.ends_with("+json")) {
      error = {NetworkErrorCode::MalformedResponse,
               "Bootstrap response has an unsupported content type"};
      return false;
    }
    break;
  }
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
      protocol_number != std::floor(protocol_number) ||
      minimum_number != std::floor(minimum_number) ||
      protocol_number > std::numeric_limits<std::uint32_t>::max() ||
      minimum_number > std::numeric_limits<std::uint32_t>::max()) {
    error = {NetworkErrorCode::MalformedResponse, "Bootstrap protocol versions are invalid"};
    return false;
  }
  bootstrap.protocol_version = static_cast<std::uint32_t>(protocol_number);
  bootstrap.minimum_client_protocol = static_cast<std::uint32_t>(minimum_number);
  const auto* service_version = root.find("serviceVersion");
  const auto* server_time = root.find("serverTime");
  const auto* maintenance = root.find("maintenance");
  const auto* authentication_required = root.find("authenticationRequired");
  if ((service_version && !service_version->is_string()) ||
      (server_time && !server_time->is_string()) ||
      (maintenance && maintenance->type() != core::JsonValue::Type::Bool) ||
      (authentication_required &&
       authentication_required->type() != core::JsonValue::Type::Bool)) {
    error = {NetworkErrorCode::MalformedResponse,
             "Bootstrap metadata fields have invalid types"};
    return false;
  }
  bootstrap.service_version = service_version ? service_version->as_string() : std::string{};
  bootstrap.server_time = server_time ? server_time->as_string() : std::string{};
  if (!valid_protocol_text(bootstrap.service_version, kMaximumIdentifierBytes) ||
      !valid_protocol_text(bootstrap.server_time, 128u)) {
    error = {NetworkErrorCode::MalformedResponse,
             "Bootstrap metadata exceeds protocol bounds"};
    return false;
  }
  bootstrap.maintenance = maintenance ? maintenance->as_bool() : false;
  bootstrap.authentication_required =
      authentication_required ? authentication_required->as_bool() : false;

  std::vector<std::string> advertised;
  if (!parse_string_array(root.find("capabilities"), advertised, error) ||
      !parse_string_array(root.find("requiredCapabilities"), bootstrap.required_capabilities,
                          error)) {
    return false;
  }
  for (const auto& capability : bootstrap.required_capabilities) {
    if (!known_capability(capability)) {
      error = {NetworkErrorCode::UnsupportedCapability,
               "Server requires an unsupported capability"};
      return false;
    }
    if (!bootstrap.capabilities.contains(capability)) {
      bootstrap.capabilities.negotiated.push_back(capability);
    }
  }
  for (const auto& capability : advertised) {
    if (known_capability(capability) && !bootstrap.capabilities.contains(capability)) {
      bootstrap.capabilities.negotiated.push_back(capability);
    }
  }

  if (const auto* endpoints = root.find("endpoints"); endpoints != nullptr) {
    if (!endpoints->is_object()) {
      error = {NetworkErrorCode::MalformedResponse, "Bootstrap endpoints must be an object"};
      return false;
    }
    const auto* realtime = endpoints->find("realtime");
    const auto* relay = endpoints->find("relay");
    if ((realtime && !realtime->is_string()) || (relay && !relay->is_string())) {
      error = {NetworkErrorCode::MalformedResponse,
               "Bootstrap endpoint fields must be strings"};
      return false;
    }
    bootstrap.discovered_endpoints.realtime_url =
        realtime ? realtime->as_string() : std::string{};
    bootstrap.discovered_endpoints.relay_url = relay ? relay->as_string() : std::string{};
    if (bootstrap.discovered_endpoints.realtime_url.size() > 2048u ||
        bootstrap.discovered_endpoints.relay_url.size() > 2048u) {
      error = {NetworkErrorCode::MalformedResponse,
               "Discovered endpoint exceeds protocol bounds"};
      return false;
    }
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
  if (!authorized_discovered_endpoint(parsed.discovered_endpoints.realtime_url,
                                      config_.endpoints.realtime_url,
                                      config_.endpoints.base_url, config_.environment, true) ||
      !authorized_discovered_endpoint(parsed.discovered_endpoints.relay_url,
                                      config_.endpoints.relay_url,
                                      config_.endpoints.base_url, config_.environment, false)) {
    error = {NetworkErrorCode::TlsFailure,
             "Bootstrap returned an endpoint outside the configured trusted origin"};
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
