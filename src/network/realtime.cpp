#include "xenon/network/realtime.hpp"

#include <algorithm>
#include <utility>

namespace xenon::network {

NetworkRealtimeController::NetworkRealtimeController(
    std::shared_ptr<NetworkTransport> scheduler,
    std::shared_ptr<NetworkRealtimeChannel> channel, RetryPolicy retry_policy)
    : scheduler_(std::move(scheduler)),
      channel_(std::move(channel)),
      retry_policy_(retry_policy) {}

NetworkRealtimeController::~NetworkRealtimeController() { stop(); }

void NetworkRealtimeController::start(std::string endpoint, std::string access_token,
                                      StateCallback state_callback,
                                      EventCallback event_callback) {
  stop();
  {
    std::scoped_lock lock(mutex_);
    cancellation_ = CancellationSource{};
    endpoint_ = std::move(endpoint);
    access_token_ = std::move(access_token);
    state_callback_ = std::move(state_callback);
    event_callback_ = std::move(event_callback);
    retry_count_ = 0;
    reconnect_count_ = 0;
    stopped_ = false;
  }
  connect_once(false);
}

void NetworkRealtimeController::connect_once(bool reconnecting) {
  StateCallback callback;
  std::string endpoint;
  std::string token;
  EventCallback event_callback;
  CancellationToken cancellation;
  {
    std::scoped_lock lock(mutex_);
    if (stopped_ || !channel_) return;
    state_ = reconnecting ? NetworkRealtimeState::Reconnecting
                          : NetworkRealtimeState::Connecting;
    callback = state_callback_;
    endpoint = endpoint_;
    token = access_token_;
    event_callback = event_callback_;
    cancellation = cancellation_.token();
  }
  if (callback) callback(state(), {});
  const auto weak = weak_from_this();
  channel_->connect(
      std::move(endpoint), std::move(token), cancellation,
      [weak](NetworkRealtimeState next, NetworkError error) {
        if (const auto self = weak.lock()) self->handle_state(next, std::move(error));
      },
      [weak, event_callback = std::move(event_callback)](NetworkRealtimeEvent event) mutable {
        if (!weak.expired() && event_callback) event_callback(std::move(event));
      });
}

void NetworkRealtimeController::handle_state(NetworkRealtimeState next, NetworkError error) {
  if (next == NetworkRealtimeState::Disconnected && !error) {
    error = {NetworkErrorCode::ConnectionFailure, "Realtime channel disconnected"};
  }
  StateCallback callback;
  bool reconnect = false;
  std::chrono::milliseconds delay{0};
  {
    std::scoped_lock lock(mutex_);
    if (stopped_) return;
    state_ = next;
    callback = state_callback_;
    if (next == NetworkRealtimeState::Connected) {
      retry_count_ = 0;
    } else if ((next == NetworkRealtimeState::Disconnected ||
                next == NetworkRealtimeState::Error) &&
               error.retryable() && retry_count_ < retry_policy_.maximum_retries) {
      delay = retry_policy_.delay_for(retry_count_, reconnect_count_ + 1u);
      ++retry_count_;
      ++reconnect_count_;
      reconnect = true;
    } else if (next == NetworkRealtimeState::Disconnected ||
               next == NetworkRealtimeState::Error) {
      state_ = NetworkRealtimeState::Unavailable;
      next = state_;
    }
  }
  if (callback) callback(next, error);
  if (!reconnect || !scheduler_) return;
  const auto weak = weak_from_this();
  scheduler_->schedule(delay, cancellation_.token(), [weak]() {
    if (const auto self = weak.lock()) self->connect_once(true);
  });
}

void NetworkRealtimeController::stop() {
  std::shared_ptr<NetworkRealtimeChannel> channel;
  StateCallback callback;
  {
    std::scoped_lock lock(mutex_);
    if (stopped_) return;
    stopped_ = true;
    cancellation_.cancel();
    channel = channel_;
    callback = state_callback_;
    state_ = NetworkRealtimeState::Disconnected;
    std::fill(access_token_.begin(), access_token_.end(), '\0');
    access_token_.clear();
    endpoint_.clear();
  }
  if (channel) channel->disconnect();
  if (callback) callback(NetworkRealtimeState::Disconnected, {});
}

NetworkRealtimeState NetworkRealtimeController::state() const {
  std::scoped_lock lock(mutex_);
  return state_;
}

std::uint64_t NetworkRealtimeController::reconnect_count() const {
  std::scoped_lock lock(mutex_);
  return reconnect_count_;
}

}  // namespace xenon::network
