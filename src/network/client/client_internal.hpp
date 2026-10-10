#pragma once

// Private to XenonNetworkClient: endpoint and protocol helpers shared by the
// client's translation units, and the in-flight request record.

#include "xenon/network/client.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#include <utility>

#include "xenon/logging/logger.hpp"

namespace xenon::network {
namespace client_detail {

struct ParsedEndpoint {
  std::string scheme{};
  std::string host{};
  std::uint16_t port{0};
};

// endpoint.cpp
std::string lowercase(std::string value);
bool parse_endpoint(std::string_view url, ParsedEndpoint& parsed);
bool is_loopback_host(const ParsedEndpoint& endpoint);
bool secure_discovered_endpoint(std::string_view url, NetworkEnvironment environment,
                                bool realtime);
bool authorized_discovered_endpoint(std::string_view discovered,
                                    std::string_view configured,
                                    std::string_view base_url,
                                    NetworkEnvironment environment,
                                    bool realtime);

// protocol.cpp
// Sequence behind request_id(); handle_result() also passes it to the retry
// delay calculation.
extern std::atomic<std::uint64_t> g_request_counter;
std::string request_id();
std::string contact_timestamp();
NetworkError http_error(const NetworkResponse& response);
bool valid_protocol_text(std::string_view value, std::size_t maximum_bytes);
bool parse_string_array(const core::JsonValue* value, std::vector<std::string>& output,
                        NetworkError& error);
bool known_capability(std::string_view capability);
NetworkError validate_operation(const NetworkOperation& operation, const NetworkConfig& config);
bool valid_auth_session(const AuthSession& session);

}  // namespace client_detail

struct XenonNetworkClient::PendingRequest {
  NetworkOperation operation{};
  Completion completion{};
  std::uint32_t retry_count{0};
  bool bootstrap_request{false};
  std::chrono::steady_clock::time_point started_at{std::chrono::steady_clock::now()};
};

}  // namespace xenon::network
