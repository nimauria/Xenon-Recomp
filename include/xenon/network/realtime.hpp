#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "xenon/network/transport.hpp"

namespace xenon::network {

// Owns bounded reconnect policy independently from the concrete persistent
// transport (WebSocket or a future alternative). It never assumes that a
// dropped channel preserved presence or session membership.
class NetworkRealtimeController final
    : public std::enable_shared_from_this<NetworkRealtimeController> {
 public:
  using StateCallback = NetworkRealtimeChannel::StateCallback;
  using EventCallback = NetworkRealtimeChannel::EventCallback;

  NetworkRealtimeController(std::shared_ptr<NetworkTransport> scheduler,
                            std::shared_ptr<NetworkRealtimeChannel> channel,
                            RetryPolicy retry_policy = {});
  ~NetworkRealtimeController();

  void start(std::string endpoint, std::string access_token,
             StateCallback state_callback, EventCallback event_callback);
  void stop();
  [[nodiscard]] NetworkRealtimeState state() const;
  [[nodiscard]] std::uint64_t reconnect_count() const;

 private:
  void connect_once(bool reconnecting);
  void handle_state(NetworkRealtimeState state, NetworkError error);

  std::shared_ptr<NetworkTransport> scheduler_;
  std::shared_ptr<NetworkRealtimeChannel> channel_;
  RetryPolicy retry_policy_;
  mutable std::mutex mutex_;
  CancellationSource cancellation_{};
  std::string endpoint_{};
  std::string access_token_{};
  StateCallback state_callback_{};
  EventCallback event_callback_{};
  NetworkRealtimeState state_{NetworkRealtimeState::Disconnected};
  std::uint32_t retry_count_{0};
  std::uint64_t reconnect_count_{0};
  bool stopped_{true};
};

}  // namespace xenon::network
