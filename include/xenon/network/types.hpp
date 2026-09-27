#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "xenon/core/json.hpp"

namespace xenon::network {

inline constexpr std::uint32_t kClientProtocolVersion = 1;
inline constexpr std::uint32_t kMinimumClientProtocolVersion = 1;
inline constexpr std::size_t kDefaultMaximumResponseBytes = 1024u * 1024u;
inline constexpr std::size_t kDefaultMaximumRequestBytes = 1024u * 1024u;
inline constexpr std::size_t kDefaultMaximumRealtimeEventBytes = 256u * 1024u;
inline constexpr std::size_t kMaximumIdentifierBytes = 256u;
inline constexpr std::size_t kMaximumCapabilities = 128u;
inline constexpr std::size_t kMaximumCredentialBytes = 8192u;

enum class NetworkEnvironment : std::uint8_t { Offline, Development, Production };
enum class NetworkConnectionState : std::uint8_t {
  Offline,
  Disabled,
  Resolving,
  Connecting,
  ConnectedTransport,
  Authenticating,
  Ready,
  Degraded,
  Reconnecting,
  Unavailable,
  Error,
};
enum class NetworkAuthState : std::uint8_t {
  Unauthenticated,
  Authenticating,
  Authenticated,
  Refreshing,
  Expired,
  Revoked,
  Error,
};
enum class NetworkRealtimeState : std::uint8_t {
  Disconnected,
  Connecting,
  Connected,
  Reconnecting,
  Unavailable,
  Error,
};
enum class NetworkErrorCode : std::uint8_t {
  None,
  Offline,
  Disabled,
  EndpointNotConfigured,
  InvalidConfiguration,
  DnsFailure,
  ConnectionFailure,
  TlsFailure,
  Timeout,
  Cancelled,
  AuthenticationRequired,
  AuthenticationExpired,
  Forbidden,
  NotFound,
  Conflict,
  RateLimited,
  ServiceUnavailable,
  ProtocolMismatch,
  UnsupportedCapability,
  MalformedResponse,
  RequestTooLarge,
  ResponseTooLarge,
  ServerError,
  NotImplemented,
};
enum class NetworkHttpMethod : std::uint8_t { Get, Post, Put, Patch, Delete };
enum class SessionVisibility : std::uint8_t { Private, Friends, Public };
enum class SessionRole : std::uint8_t { Member, Host };
enum class ConnectivityMode : std::uint8_t { Direct, Relay, Unavailable };

struct NetworkEndpointSet {
  std::string base_url{};
  std::string realtime_url{};
  std::string relay_url{};
};

struct RetryPolicy {
  std::uint32_t maximum_retries{2};
  std::chrono::milliseconds initial_backoff{250};
  std::chrono::milliseconds maximum_backoff{4000};
  double jitter_ratio{0.20};

  [[nodiscard]] std::chrono::milliseconds delay_for(std::uint32_t retry_index,
                                                     std::uint64_t jitter_seed) const;
};

struct NetworkConfig {
  bool enabled{false};
  NetworkEnvironment environment{NetworkEnvironment::Offline};
  NetworkEndpointSet endpoints{};
  std::chrono::milliseconds connect_timeout{5000};
  std::chrono::milliseconds request_timeout{10000};
  std::chrono::milliseconds realtime_heartbeat_timeout{30000};
  std::size_t maximum_request_bytes{kDefaultMaximumRequestBytes};
  std::size_t maximum_response_bytes{kDefaultMaximumResponseBytes};
  std::size_t maximum_queued_events{256};
  std::size_t maximum_pending_requests{64};
  RetryPolicy retry{};
};

struct NetworkError {
  NetworkErrorCode code{NetworkErrorCode::None};
  std::string diagnostic{};
  int http_status{0};
  std::chrono::milliseconds retry_after{0};

  [[nodiscard]] bool retryable() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept { return code != NetworkErrorCode::None; }
};

struct NetworkRequest {
  std::string request_id{};
  std::string route_name{};
  NetworkHttpMethod method{NetworkHttpMethod::Get};
  std::string url{};
  std::map<std::string, std::string> headers{};
  std::string body{};
  std::string idempotency_key{};
  std::chrono::milliseconds connect_timeout{0};
  std::chrono::milliseconds request_timeout{0};
  std::size_t maximum_response_bytes{kDefaultMaximumResponseBytes};
};

struct NetworkResponse {
  int status_code{0};
  std::map<std::string, std::string> headers{};
  std::string body{};
  std::size_t bytes_sent{0};
  std::size_t bytes_received{0};
};

struct NetworkResult {
  NetworkResponse response{};
  NetworkError error{};
  [[nodiscard]] bool ok() const noexcept { return !error; }
};

struct NetworkCapabilities {
  std::vector<std::string> negotiated{};
  [[nodiscard]] bool contains(std::string_view capability) const;
};

struct NetworkBootstrap {
  std::uint32_t protocol_version{0};
  std::uint32_t minimum_client_protocol{0};
  std::string service_version{};
  std::string server_time{};
  NetworkCapabilities capabilities{};
  std::vector<std::string> required_capabilities{};
  NetworkEndpointSet discovered_endpoints{};
  bool maintenance{false};
  bool authentication_required{false};
};

struct NetworkTitleContext {
  std::uint32_t title_id{0};
  std::string title_version{};
  std::string title_update_identity{};
  std::string module_id{};
  std::string module_version{};
  std::string runtime_session_id{};
};

struct NetworkClientIdentity {
  std::string xenon_version{};
  std::string host_platform{};
  std::string host_architecture{};
  std::vector<std::string> requested_capabilities{};
  std::optional<NetworkTitleContext> title{};

  [[nodiscard]] core::JsonValue handshake_json(std::string_view request_id) const;
};

using NetworkSessionId = std::string;
using NetworkUserId = std::string;

struct SessionMember {
  NetworkUserId user_id{};
  SessionRole role{SessionRole::Member};
  bool confirmed{false};
};

struct NetworkSessionState {
  NetworkSessionId session_id{};
  SessionVisibility visibility{SessionVisibility::Private};
  std::vector<SessionMember> members{};
  std::optional<NetworkUserId> host_member{};
  std::uint32_t maximum_members{0};
  std::string join_token{};
};

struct ConnectivityPlan {
  ConnectivityMode mode{ConnectivityMode::Unavailable};
  NetworkSessionId session_id{};
  std::vector<std::string> peer_endpoints{};
  std::string relay_endpoint{};
  std::string connection_token{};
  std::string expires_at{};
};

struct NetworkMetrics {
  std::uint64_t requests_attempted{0};
  std::uint64_t requests_completed{0};
  std::uint64_t requests_failed{0};
  std::uint64_t timeouts{0};
  std::uint64_t retries{0};
  std::uint64_t reconnects{0};
  std::uint64_t bytes_sent{0};
  std::uint64_t bytes_received{0};
  std::uint64_t realtime_reconnects{0};
};

struct NetworkStatus {
  NetworkConnectionState connection{NetworkConnectionState::Offline};
  NetworkAuthState authentication{NetworkAuthState::Unauthenticated};
  NetworkRealtimeState realtime{NetworkRealtimeState::Disconnected};
  bool transport_available{false};
  bool configured{false};
  bool service_reachable{false};
  bool protocol_compatible{false};
  NetworkCapabilities capabilities{};
  std::string last_successful_contact{};
  NetworkError last_error{};
};

class CancellationToken {
 public:
  CancellationToken() = default;
  [[nodiscard]] bool cancelled() const noexcept;

 private:
  friend class CancellationSource;
  explicit CancellationToken(std::shared_ptr<std::atomic_bool> state) : state_(std::move(state)) {}
  std::shared_ptr<std::atomic_bool> state_{};
};

class CancellationSource {
 public:
  CancellationSource();
  [[nodiscard]] CancellationToken token() const { return CancellationToken(state_); }
  void cancel() noexcept;

 private:
  std::shared_ptr<std::atomic_bool> state_;
};

struct AuthSession {
  std::string access_token{};
  std::string refresh_token{};
  std::string expires_at{};
};

class CredentialStore {
 public:
  virtual ~CredentialStore() = default;
  [[nodiscard]] virtual std::optional<AuthSession> load() = 0;
  virtual bool store(const AuthSession& session) = 0;
  virtual void clear() = 0;
  [[nodiscard]] virtual bool persistent() const noexcept = 0;
};

// V1 deliberately keeps credentials in process memory. Platform keychain
// implementations can replace this without changing the client or protocol.
class MemoryCredentialStore final : public CredentialStore {
 public:
  ~MemoryCredentialStore() override;
  [[nodiscard]] std::optional<AuthSession> load() override;
  bool store(const AuthSession& session) override;
  void clear() override;
  [[nodiscard]] bool persistent() const noexcept override { return false; }

 private:
  std::mutex mutex_;
  std::optional<AuthSession> session_{};
};

[[nodiscard]] std::string_view to_string(NetworkConnectionState state) noexcept;
[[nodiscard]] std::string_view to_string(NetworkAuthState state) noexcept;
[[nodiscard]] std::string_view to_string(NetworkRealtimeState state) noexcept;
[[nodiscard]] std::string_view to_string(NetworkErrorCode code) noexcept;
[[nodiscard]] bool valid_utf8(std::string_view value) noexcept;
[[nodiscard]] bool valid_http_header_value(std::string_view value,
                                           std::size_t maximum_bytes) noexcept;
void secure_erase(std::string& value) noexcept;
[[nodiscard]] std::string sanitize_for_log(std::string_view value);
[[nodiscard]] core::JsonValue network_status_json(const NetworkStatus& status,
                                                  const NetworkMetrics& metrics);

}  // namespace xenon::network
