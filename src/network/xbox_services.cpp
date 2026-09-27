#include "xenon/network/xbox_services.hpp"

#include <utility>

namespace xenon::network {

NetworkError XboxServicesNetworkAdapter::availability(
    std::string_view capability, bool authentication_required) const {
  if (!client_) {
    return {NetworkErrorCode::ServiceUnavailable, "Xenon Network client is unavailable"};
  }
  const auto status = client_->status();
  switch (status.connection) {
    case NetworkConnectionState::Offline:
      return {NetworkErrorCode::Offline, "Xenon is operating offline"};
    case NetworkConnectionState::Disabled:
      return {NetworkErrorCode::Disabled, "Xenon Network is disabled"};
    case NetworkConnectionState::Unavailable:
    case NetworkConnectionState::Error:
      return status.last_error ? status.last_error
                               : NetworkError{NetworkErrorCode::ServiceUnavailable,
                                              "Xenon Network service is unavailable"};
    case NetworkConnectionState::Resolving:
    case NetworkConnectionState::Connecting:
    case NetworkConnectionState::ConnectedTransport:
    case NetworkConnectionState::Authenticating:
    case NetworkConnectionState::Reconnecting:
      return {NetworkErrorCode::ServiceUnavailable, "Xenon Network is not ready"};
    case NetworkConnectionState::Degraded:
    case NetworkConnectionState::Ready: break;
  }
  if (!capability.empty() && !status.capabilities.contains(capability)) {
    return {NetworkErrorCode::NotImplemented,
            "The Xenon Network service did not advertise this capability"};
  }
  if (authentication_required && status.authentication != NetworkAuthState::Authenticated) {
    return {NetworkErrorCode::AuthenticationRequired,
            "The Xbox online operation requires a Xenon Network identity"};
  }
  return {};
}

void XboxServicesNetworkAdapter::perform(
    std::string_view capability, bool authentication_required, NetworkOperation operation,
    XenonNetworkClient::Completion completion) const {
  const auto error = availability(capability, authentication_required);
  if (error) {
    if (completion) completion(NetworkResult{{}, error});
    return;
  }
  operation.authenticated = authentication_required;
  client_->perform(std::move(operation), std::move(completion));
}

}  // namespace xenon::network
