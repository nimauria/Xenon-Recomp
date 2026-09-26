#include "xenon/network/realtime.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <utility>

namespace xenon::network {
namespace {

bool valid_opaque_token(std::string_view value) {
  return !value.empty() && value.size() <= kMaximumCredentialBytes &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return c >= 0x21u && c <= 0x7Eu;
         });
}

bool valid_event_identifier(std::string_view value) {
  return !value.empty() && value.size() <= kMaximumIdentifierBytes &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == ':';
         });
}

}  // namespace

NetworkRealtimeController::NetworkRealtimeController(
    std::shared_ptr<NetworkTransport> scheduler,
    std::shared_ptr<NetworkRealtimeChannel> channel, RetryPolicy retry_policy,
    std::size_t maximum_event_bytes)
    : scheduler_(std::move(scheduler)),
      channel_(std::move(channel)),
      retry_policy_(retry_policy),
      maximum_event_bytes_(std::clamp<std::size_t>(maximum_event_bytes, 1u,
                                                   1024u * 1024u)) {}

NetworkRealtimeController::~NetworkRealtimeController() { stop(); }

void NetworkRealtimeController::start(std::string endpoint, std::string access_token,
                                      StateCallback state_callback,
                                      EventCallback event_callback) {
  stop();
  const bool endpoint_has_invalid_character =
      std::any_of(endpoint.begin(), endpoint.end(), [](unsigned char c) {
        return c < 0x21u || c > 0x7Eu;
      });
  if (!scheduler_ || !channel_ || endpoint.empty() || endpoint.size() > 2048u ||
      !valid_utf8(endpoint) || endpoint_has_invalid_character ||
      (!access_token.empty() && !valid_opaque_token(access_token))) {
    secure_erase(access_token);
    {
      std::scoped_lock lock(mutex_);
      state_ = NetworkRealtimeState::Unavailable;
    }
    if (state_callback) {
      state_callback(NetworkRealtimeState::Unavailable,
                     {NetworkErrorCode::InvalidConfiguration,
                      "Realtime channel configuration is invalid"});
    }
    return;
  }
  std::uint64_t generation = 0;
  {
    std::scoped_lock lock(mutex_);
    generation = ++generation_;
    cancellation_ = CancellationSource{};
    endpoint_ = std::move(endpoint);
    access_token_ = std::move(access_token);
    state_callback_ = std::move(state_callback);
    event_callback_ = std::move(event_callback);
    retry_count_ = 0;
    reconnect_count_ = 0;
    stopped_ = false;
  }
  connect_once(false, generation);
}

void NetworkRealtimeController::connect_once(bool reconnecting, std::uint64_t generation) {
  StateCallback callback;
  std::string endpoint;
  std::string token;
  CancellationToken cancellation;
  {
    std::scoped_lock lock(mutex_);
    if (stopped_ || generation != generation_ || !channel_) return;
    state_ = reconnecting ? NetworkRealtimeState::Reconnecting
                          : NetworkRealtimeState::Connecting;
    callback = state_callback_;
    endpoint = endpoint_;
    token = access_token_;
    cancellation = cancellation_.token();
  }
  if (callback) callback(state(), {});
  const auto weak = weak_from_this();
  channel_->connect(
      std::move(endpoint), std::move(token), cancellation,
      [weak, generation](NetworkRealtimeState next, NetworkError error) {
        if (const auto self = weak.lock()) {
          self->handle_state(generation, next, std::move(error));
        }
      },
      [weak, generation](NetworkRealtimeEvent event) mutable {
        if (const auto self = weak.lock()) {
          self->handle_event(generation, std::move(event));
        }
      });
}

void NetworkRealtimeController::handle_event(std::uint64_t generation,
                                             NetworkRealtimeEvent event) {
  EventCallback event_callback;
  StateCallback state_callback;
  std::shared_ptr<NetworkRealtimeChannel> channel;
  bool invalid = false;
  {
    std::scoped_lock lock(mutex_);
    if (stopped_ || generation != generation_ ||
        state_ != NetworkRealtimeState::Connected) {
      return;
    }
    invalid = !valid_event_identifier(event.event_id) ||
              !valid_event_identifier(event.event_type) ||
              event.payload.size() > maximum_event_bytes_ || !valid_utf8(event.payload);
    if (!invalid) {
      event_callback = event_callback_;
    } else {
      stopped_ = true;
      cancellation_.cancel();
      state_ = NetworkRealtimeState::Unavailable;
      state_callback = state_callback_;
      channel = channel_;
      secure_erase(access_token_);
      endpoint_.clear();
    }
  }
  if (invalid) {
    if (channel) channel->disconnect();
    if (state_callback) {
      state_callback(NetworkRealtimeState::Unavailable,
                     {NetworkErrorCode::MalformedResponse,
                      "Realtime event violated protocol limits"});
    }
    return;
  }
  if (event_callback) event_callback(std::move(event));
}

void NetworkRealtimeController::handle_state(std::uint64_t generation,
                                             NetworkRealtimeState next,
                                             NetworkError error) {
  if (next == NetworkRealtimeState::Disconnected && !error) {
    error = {NetworkErrorCode::ConnectionFailure, "Realtime channel disconnected"};
  }
  StateCallback callback;
  bool reconnect = false;
  std::chrono::milliseconds delay{0};
  {
    std::scoped_lock lock(mutex_);
    if (stopped_ || generation != generation_) return;
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
  scheduler_->schedule(delay, cancellation_.token(), [weak, generation]() {
    if (const auto self = weak.lock()) self->connect_once(true, generation);
  });
}

void NetworkRealtimeController::stop() {
  std::shared_ptr<NetworkRealtimeChannel> channel;
  StateCallback callback;
  {
    std::scoped_lock lock(mutex_);
    if (stopped_) return;
    stopped_ = true;
    ++generation_;
    cancellation_.cancel();
    channel = channel_;
    callback = state_callback_;
    state_ = NetworkRealtimeState::Disconnected;
    secure_erase(access_token_);
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
