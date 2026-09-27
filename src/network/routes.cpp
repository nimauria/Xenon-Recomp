#include "xenon/network/routes.hpp"

#include <array>
#include <cctype>
#include <stdexcept>

namespace xenon::network {
namespace {

constexpr std::array<RouteDefinition, 23> kRoutes{{
    {NetworkRoute::Bootstrap, "bootstrap", NetworkHttpMethod::Get, "/v1/bootstrap", false, false,
     false},
    {NetworkRoute::Health, "health", NetworkHttpMethod::Get, "/v1/health", false, false, false},
    {NetworkRoute::ClientHandshake, "client.handshake", NetworkHttpMethod::Post,
     "/v1/client/handshake", false, false, true},
    {NetworkRoute::AuthSessionCreate, "auth.session.create", NetworkHttpMethod::Post,
     "/v1/auth/session", false, false, true},
    {NetworkRoute::AuthSessionDelete, "auth.session.delete", NetworkHttpMethod::Delete,
     "/v1/auth/session", true, false, true},
    {NetworkRoute::AuthRefresh, "auth.refresh", NetworkHttpMethod::Post, "/v1/auth/refresh", false,
     false, true},
    {NetworkRoute::ProfileMe, "profile.me", NetworkHttpMethod::Get, "/v1/profile/me", true, false,
     false},
    {NetworkRoute::PresenceMeGet, "presence.me.get", NetworkHttpMethod::Get, "/v1/presence/me", true,
     false, false},
    {NetworkRoute::PresenceMePut, "presence.me.put", NetworkHttpMethod::Put, "/v1/presence/me", true,
     false, true},
    {NetworkRoute::FriendsList, "friends.list", NetworkHttpMethod::Get, "/v1/friends", true, false,
     false},
    {NetworkRoute::MatchmakingTicketCreate, "matchmaking.ticket.create", NetworkHttpMethod::Post,
     "/v1/matchmaking/tickets", true, false, true},
    {NetworkRoute::MatchmakingTicketGet, "matchmaking.ticket.get", NetworkHttpMethod::Get,
     "/v1/matchmaking/tickets/{id}", true, true, false},
    {NetworkRoute::MatchmakingTicketDelete, "matchmaking.ticket.delete", NetworkHttpMethod::Delete,
     "/v1/matchmaking/tickets/{id}", true, true, true},
    {NetworkRoute::SessionCreate, "session.create", NetworkHttpMethod::Post, "/v1/sessions", true,
     false, true},
    {NetworkRoute::SessionGet, "session.get", NetworkHttpMethod::Get, "/v1/sessions/{id}", true, true,
     false},
    {NetworkRoute::SessionPatch, "session.patch", NetworkHttpMethod::Patch, "/v1/sessions/{id}", true,
     true, true},
    {NetworkRoute::SessionDelete, "session.delete", NetworkHttpMethod::Delete, "/v1/sessions/{id}",
     true, true, true},
    {NetworkRoute::SessionJoin, "session.join", NetworkHttpMethod::Post, "/v1/sessions/{id}/join", true,
     true, true},
    {NetworkRoute::SessionLeave, "session.leave", NetworkHttpMethod::Post,
     "/v1/sessions/{id}/leave", true, true, true},
    {NetworkRoute::ConnectivityAllocationCreate, "connectivity.allocation.create",
     NetworkHttpMethod::Post, "/v1/connectivity/allocations", true, false, true},
    {NetworkRoute::ConnectivityAllocationDelete, "connectivity.allocation.delete",
     NetworkHttpMethod::Delete, "/v1/connectivity/allocations/{id}", true, true, true},
    {NetworkRoute::RealtimeEvents, "events", NetworkHttpMethod::Get, "/v1/events", true, false,
     false},
    {NetworkRoute::Bootstrap, "invalid", NetworkHttpMethod::Get, "", false, false, false},
}};

std::string encode_identifier(std::string_view value) {
  constexpr char kHex[] = "0123456789ABCDEF";
  std::string encoded;
  for (const unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded.push_back(static_cast<char>(c));
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[(c >> 4u) & 0x0Fu]);
      encoded.push_back(kHex[c & 0x0Fu]);
    }
  }
  return encoded;
}

}  // namespace

const RouteDefinition& route_definition(NetworkRoute route) {
  const auto index = static_cast<std::size_t>(route);
  if (!valid_network_route(route)) throw std::out_of_range("Unknown Xenon Network route");
  return kRoutes[index];
}

bool valid_network_route(NetworkRoute route) noexcept {
  return static_cast<std::size_t>(route) < static_cast<std::size_t>(NetworkRoute::Count);
}

bool valid_route_identifier(std::string_view identifier) noexcept {
  if (identifier.empty() || identifier.size() > kMaximumIdentifierBytes) return false;
  for (const unsigned char c : identifier) {
    if (c < 0x20u || c == 0x7Fu) return false;
  }
  return true;
}

std::string route_path(NetworkRoute route, std::string_view identifier) {
  const auto& definition = route_definition(route);
  std::string path(definition.path_template);
  if (!definition.identifier_required) return path;
  if (!valid_route_identifier(identifier)) return {};
  const auto marker = path.find("{id}");
  if (marker == std::string::npos) return {};
  path.replace(marker, 4u, encode_identifier(identifier));
  return path;
}

std::string join_endpoint(std::string_view base_url, std::string_view path) {
  if (base_url.empty() || path.empty()) return {};
  std::string result(base_url);
  while (!result.empty() && result.back() == '/') result.pop_back();
  if (path.front() != '/') result.push_back('/');
  result.append(path);
  return result;
}

}  // namespace xenon::network
