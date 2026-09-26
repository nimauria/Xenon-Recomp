#pragma once

#include <chrono>
#include <functional>
#include <string>

#include "xenon/network/types.hpp"

namespace xenon::network {

class NetworkTransport {
 public:
  using Completion = std::function<void(NetworkResult)>;
  using Task = std::function<void()>;

  virtual ~NetworkTransport() = default;
  [[nodiscard]] virtual bool available() const noexcept = 0;
  virtual void send(NetworkRequest request, CancellationToken cancellation,
                    Completion completion) = 0;
  // A scheduled task is dispatched exactly once even if cancellation wins;
  // the owner uses the token to settle its callback without issuing I/O.
  virtual void schedule(std::chrono::milliseconds delay, CancellationToken cancellation,
                        Task task) = 0;
  virtual void shutdown() = 0;
};

struct NetworkRealtimeEvent {
  std::string event_id{};
  std::string event_type{};
  std::string payload{};
};

class NetworkRealtimeChannel {
 public:
  using StateCallback = std::function<void(NetworkRealtimeState, NetworkError)>;
  using EventCallback = std::function<void(NetworkRealtimeEvent)>;

  virtual ~NetworkRealtimeChannel() = default;
  virtual void connect(std::string endpoint, std::string access_token,
                       CancellationToken cancellation, StateCallback state_callback,
                       EventCallback event_callback) = 0;
  virtual void disconnect() = 0;
};

}  // namespace xenon::network
