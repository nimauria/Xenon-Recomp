#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "xenon/network/types.hpp"

namespace xenon::network {

enum class NetworkRoute : std::uint8_t {
  Bootstrap,
  Health,
  ClientHandshake,
  AuthSessionCreate,
  AuthSessionDelete,
  AuthRefresh,
  ProfileMe,
  PresenceMeGet,
  PresenceMePut,
  FriendsList,
  MatchmakingTicketCreate,
  MatchmakingTicketGet,
  MatchmakingTicketDelete,
  SessionCreate,
  SessionGet,
  SessionPatch,
  SessionDelete,
  SessionJoin,
  SessionLeave,
  ConnectivityAllocationCreate,
  ConnectivityAllocationDelete,
  RealtimeEvents,
};

struct RouteDefinition {
  NetworkRoute route;
  std::string_view name;
  NetworkHttpMethod method;
  std::string_view path_template;
  bool identifier_required;
  bool supports_idempotency;
};

[[nodiscard]] const RouteDefinition& route_definition(NetworkRoute route);
[[nodiscard]] std::string route_path(NetworkRoute route, std::string_view identifier = {});
[[nodiscard]] std::string join_endpoint(std::string_view base_url, std::string_view path);
[[nodiscard]] bool valid_route_identifier(std::string_view identifier) noexcept;

}  // namespace xenon::network
