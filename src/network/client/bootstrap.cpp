// Parsing and applying the service bootstrap document.

#include "network/client/client_internal.hpp"

namespace xenon::network {

using namespace client_detail;

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

}  // namespace xenon::network
