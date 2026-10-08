// Request identifiers, HTTP error mapping and validation of operations,
// protocol fields and auth sessions.

#include "network/client/client_internal.hpp"

namespace xenon::network::client_detail {
namespace {

bool valid_protocol_identifier(std::string_view value) {
  if (value.empty() || value.size() > kMaximumIdentifierBytes) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == ':';
  });
}

}  // namespace

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

}  // namespace xenon::network::client_detail
