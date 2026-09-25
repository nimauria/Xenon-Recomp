#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "xenon/network/routes.hpp"
#include "xenon/network/transport.hpp"

namespace xenon::network {

struct NetworkOperation {
  NetworkRoute route{NetworkRoute::Health};
  std::string identifier{};
  std::string body{};
  std::string idempotency_key{};
  bool authenticated{false};
};

class XenonNetworkClient final : public std::enable_shared_from_this<XenonNetworkClient> {
 public:
  using Completion = NetworkTransport::Completion;
  using StateCallback = std::function<void(const NetworkStatus&)>;

  XenonNetworkClient(std::shared_ptr<NetworkTransport> transport, NetworkConfig config,
                     NetworkClientIdentity identity,
                     std::shared_ptr<CredentialStore> credentials =
                         std::make_shared<MemoryCredentialStore>());
  ~XenonNetworkClient();

  XenonNetworkClient(const XenonNetworkClient&) = delete;
  XenonNetworkClient& operator=(const XenonNetworkClient&) = delete;

  void set_state_callback(StateCallback callback);
  void start(Completion completion = {});
  void check_health(Completion completion);
  void perform(NetworkOperation operation, Completion completion);
  void shutdown();

  [[nodiscard]] NetworkStatus status() const;
  [[nodiscard]] NetworkMetrics metrics() const;
  [[nodiscard]] std::optional<NetworkBootstrap> bootstrap() const;
  [[nodiscard]] NetworkConfig config() const;

  void set_auth_session(AuthSession session);
  void clear_auth_session(NetworkAuthState state = NetworkAuthState::Unauthenticated);

  [[nodiscard]] static NetworkError validate_config(const NetworkConfig& config);
  [[nodiscard]] static bool parse_bootstrap(const NetworkResponse& response,
                                            NetworkBootstrap& bootstrap,
                                            NetworkError& error);

 private:
  struct PendingRequest;

  void transition(NetworkConnectionState state, NetworkError error = {});
  void issue(PendingRequest pending);
  void handle_result(PendingRequest pending, NetworkResult result);
  [[nodiscard]] NetworkRequest make_request(const NetworkOperation& operation) const;
  [[nodiscard]] bool apply_bootstrap(const NetworkResponse& response, NetworkError& error);
  [[nodiscard]] std::string access_token() const;

  std::shared_ptr<NetworkTransport> transport_;
  NetworkConfig config_;
  NetworkClientIdentity identity_;
  std::shared_ptr<CredentialStore> credentials_;
  mutable std::mutex mutex_;
  NetworkStatus status_{};
  NetworkMetrics metrics_{};
  std::optional<NetworkBootstrap> bootstrap_{};
  StateCallback state_callback_{};
  CancellationSource cancellation_{};
  bool shutdown_{false};
};

}  // namespace xenon::network
