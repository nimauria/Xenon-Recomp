#include "xenon/network/types.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

namespace xenon::network {

std::chrono::milliseconds RetryPolicy::delay_for(std::uint32_t retry_index,
                                                  std::uint64_t jitter_seed) const {
  const auto shift = std::min<std::uint32_t>(retry_index, 20u);
  const auto multiplier = std::uint64_t{1} << shift;
  const auto raw = std::chrono::milliseconds(
      std::min<std::uint64_t>(static_cast<std::uint64_t>(maximum_backoff.count()),
                              static_cast<std::uint64_t>(initial_backoff.count()) * multiplier));
  const double bounded_jitter = std::clamp(jitter_ratio, 0.0, 1.0);
  const double unit = static_cast<double>(jitter_seed % 10001u) / 10000.0;
  const double factor = (1.0 - bounded_jitter) + (2.0 * bounded_jitter * unit);
  return std::chrono::milliseconds(
      std::max<std::int64_t>(0, static_cast<std::int64_t>(std::llround(raw.count() * factor))));
}

bool NetworkError::retryable() const noexcept {
  switch (code) {
    case NetworkErrorCode::DnsFailure:
    case NetworkErrorCode::ConnectionFailure:
    case NetworkErrorCode::Timeout:
    case NetworkErrorCode::RateLimited:
    case NetworkErrorCode::ServiceUnavailable:
    case NetworkErrorCode::ServerError: return true;
    default: return false;
  }
}

bool NetworkCapabilities::contains(std::string_view capability) const {
  return std::find(negotiated.begin(), negotiated.end(), capability) != negotiated.end();
}

core::JsonValue NetworkClientIdentity::handshake_json(std::string_view request_id) const {
  core::JsonValue result = core::JsonValue::make_object();
  result.set("protocolVersion", kClientProtocolVersion);
  result.set("xenonVersion", xenon_version);
  result.set("hostPlatform", host_platform);
  result.set("hostArchitecture", host_architecture);
  result.set("requestId", std::string(request_id));
  core::JsonValue requested = core::JsonValue::make_array();
  for (const auto& capability : requested_capabilities) requested.append(capability);
  result.set("requestedCapabilities", std::move(requested));
  if (title) {
    core::JsonValue title_value = core::JsonValue::make_object();
    title_value.set("titleId", title->title_id);
    title_value.set("titleVersion", title->title_version);
    title_value.set("titleUpdateIdentity", title->title_update_identity);
    title_value.set("moduleId", title->module_id);
    title_value.set("moduleVersion", title->module_version);
    title_value.set("runtimeSessionId", title->runtime_session_id);
    result.set("title", std::move(title_value));
  }
  return result;
}

bool CancellationToken::cancelled() const noexcept {
  return state_ && state_->load(std::memory_order_acquire);
}

CancellationSource::CancellationSource() : state_(std::make_shared<std::atomic_bool>(false)) {}

void CancellationSource::cancel() noexcept { state_->store(true, std::memory_order_release); }

std::optional<AuthSession> MemoryCredentialStore::load() {
  std::scoped_lock lock(mutex_);
  return session_;
}

bool MemoryCredentialStore::store(const AuthSession& session) {
  std::scoped_lock lock(mutex_);
  session_ = session;
  return true;
}

void MemoryCredentialStore::clear() {
  std::scoped_lock lock(mutex_);
  if (session_) {
    std::fill(session_->access_token.begin(), session_->access_token.end(), '\0');
    std::fill(session_->refresh_token.begin(), session_->refresh_token.end(), '\0');
  }
  session_.reset();
}

std::string_view to_string(NetworkConnectionState state) noexcept {
  switch (state) {
    case NetworkConnectionState::Offline: return "offline";
    case NetworkConnectionState::Disabled: return "disabled";
    case NetworkConnectionState::Resolving: return "resolving";
    case NetworkConnectionState::Connecting: return "connecting";
    case NetworkConnectionState::ConnectedTransport: return "connected_transport";
    case NetworkConnectionState::Authenticating: return "authenticating";
    case NetworkConnectionState::Ready: return "ready";
    case NetworkConnectionState::Degraded: return "degraded";
    case NetworkConnectionState::Reconnecting: return "reconnecting";
    case NetworkConnectionState::Unavailable: return "unavailable";
    case NetworkConnectionState::Error: return "error";
  }
  return "error";
}

std::string_view to_string(NetworkAuthState state) noexcept {
  switch (state) {
    case NetworkAuthState::Unauthenticated: return "unauthenticated";
    case NetworkAuthState::Authenticating: return "authenticating";
    case NetworkAuthState::Authenticated: return "authenticated";
    case NetworkAuthState::Refreshing: return "refreshing";
    case NetworkAuthState::Expired: return "expired";
    case NetworkAuthState::Revoked: return "revoked";
    case NetworkAuthState::Error: return "error";
  }
  return "error";
}

std::string_view to_string(NetworkRealtimeState state) noexcept {
  switch (state) {
    case NetworkRealtimeState::Disconnected: return "disconnected";
    case NetworkRealtimeState::Connecting: return "connecting";
    case NetworkRealtimeState::Connected: return "connected";
    case NetworkRealtimeState::Reconnecting: return "reconnecting";
    case NetworkRealtimeState::Unavailable: return "unavailable";
    case NetworkRealtimeState::Error: return "error";
  }
  return "error";
}

std::string_view to_string(NetworkErrorCode code) noexcept {
  switch (code) {
    case NetworkErrorCode::None: return "none";
    case NetworkErrorCode::Offline: return "offline";
    case NetworkErrorCode::Disabled: return "disabled";
    case NetworkErrorCode::EndpointNotConfigured: return "endpoint_not_configured";
    case NetworkErrorCode::InvalidConfiguration: return "invalid_configuration";
    case NetworkErrorCode::DnsFailure: return "dns_failure";
    case NetworkErrorCode::ConnectionFailure: return "connection_failure";
    case NetworkErrorCode::TlsFailure: return "tls_failure";
    case NetworkErrorCode::Timeout: return "timeout";
    case NetworkErrorCode::Cancelled: return "cancelled";
    case NetworkErrorCode::AuthenticationRequired: return "authentication_required";
    case NetworkErrorCode::AuthenticationExpired: return "authentication_expired";
    case NetworkErrorCode::Forbidden: return "forbidden";
    case NetworkErrorCode::NotFound: return "not_found";
    case NetworkErrorCode::Conflict: return "conflict";
    case NetworkErrorCode::RateLimited: return "rate_limited";
    case NetworkErrorCode::ServiceUnavailable: return "service_unavailable";
    case NetworkErrorCode::ProtocolMismatch: return "protocol_mismatch";
    case NetworkErrorCode::UnsupportedCapability: return "unsupported_capability";
    case NetworkErrorCode::MalformedResponse: return "malformed_response";
    case NetworkErrorCode::ResponseTooLarge: return "response_too_large";
    case NetworkErrorCode::ServerError: return "server_error";
    case NetworkErrorCode::NotImplemented: return "not_implemented";
  }
  return "unknown";
}

std::string sanitize_for_log(std::string_view value) {
  std::string lower(value);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  for (const auto marker : {"authorization", "access_token", "refresh_token", "password",
                            "join_token", "connection_token", "bearer "}) {
    if (lower.find(marker) != std::string::npos) return "[redacted]";
  }
  constexpr std::size_t kMaximumDiagnosticBytes = 1024;
  return std::string(value.substr(0, kMaximumDiagnosticBytes));
}

core::JsonValue network_status_json(const NetworkStatus& status, const NetworkMetrics& metrics) {
  core::JsonValue result = core::JsonValue::make_object();
  result.set("clientCompiled", true);
  result.set("transportAvailable", status.transport_available);
  result.set("configured", status.configured);
  result.set("serviceReachable", status.service_reachable);
  result.set("protocolCompatible", status.protocol_compatible);
  result.set("authenticated", status.authentication == NetworkAuthState::Authenticated);
  result.set("realtimeActive", status.realtime == NetworkRealtimeState::Connected);
  result.set("onlineServicesReady", status.connection == NetworkConnectionState::Ready);
  result.set("connectionState", std::string(to_string(status.connection)));
  result.set("authenticationState", std::string(to_string(status.authentication)));
  result.set("realtimeState", std::string(to_string(status.realtime)));
  result.set("lastSuccessfulContact", status.last_successful_contact);
  result.set("lastError", std::string(to_string(status.last_error.code)));

  core::JsonValue capabilities = core::JsonValue::make_array();
  for (const auto& capability : status.capabilities.negotiated) capabilities.append(capability);
  result.set("capabilities", std::move(capabilities));

  core::JsonValue counters = core::JsonValue::make_object();
  counters.set("requestsAttempted", metrics.requests_attempted);
  counters.set("requestsCompleted", metrics.requests_completed);
  counters.set("requestsFailed", metrics.requests_failed);
  counters.set("timeouts", metrics.timeouts);
  counters.set("retries", metrics.retries);
  counters.set("reconnects", metrics.reconnects);
  counters.set("bytesSent", metrics.bytes_sent);
  counters.set("bytesReceived", metrics.bytes_received);
  counters.set("realtimeReconnects", metrics.realtime_reconnects);
  result.set("metrics", std::move(counters));
  return result;
}

}  // namespace xenon::network
