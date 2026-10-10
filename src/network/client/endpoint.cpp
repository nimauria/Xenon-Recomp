// Parsing and checking network service endpoints: URLs, hosts, ports and
// which discovered endpoints may be trusted.

#include "network/client/client_internal.hpp"

namespace xenon::network::client_detail {
namespace {

int hex_value(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

bool parse_port(std::string_view port, std::uint16_t& parsed) {
  if (port.empty() || port.size() > 5u) return false;
  std::uint32_t value = 0;
  for (const unsigned char c : port) {
    if (!std::isdigit(c)) return false;
    value = value * 10u + static_cast<std::uint32_t>(c - '0');
  }
  if (value == 0u || value > 65535u) return false;
  parsed = static_cast<std::uint16_t>(value);
  return true;
}

bool valid_dns_host(std::string_view host) {
  if (host.empty() || host.size() > 253u || host.front() == '.' || host.back() == '.') return false;
  std::size_t label_start = 0;
  while (label_start < host.size()) {
    const auto label_end = host.find('.', label_start);
    const auto length = (label_end == std::string_view::npos ? host.size() : label_end) - label_start;
    if (length == 0u || length > 63u || host[label_start] == '-' ||
        host[label_start + length - 1u] == '-') {
      return false;
    }
    for (std::size_t index = label_start; index < label_start + length; ++index) {
      const auto c = static_cast<unsigned char>(host[index]);
      if (!std::isalnum(c) && c != '-') return false;
    }
    if (label_end == std::string_view::npos) break;
    label_start = label_end + 1u;
  }
  return true;
}

bool valid_ipv6_literal(std::string_view host) {
  if (host.empty() || host.find('.') != std::string_view::npos ||
      host.find(":::") != std::string_view::npos) {
    return false;
  }
  if ((host.front() == ':' && !host.starts_with("::")) ||
      (host.back() == ':' && !host.ends_with("::"))) {
    return false;
  }
  const auto compression = host.find("::");
  if (compression != std::string_view::npos && host.find("::", compression + 2u) != std::string_view::npos) {
    return false;
  }
  std::size_t groups = 0;
  std::size_t start = 0;
  while (start <= host.size()) {
    const auto end = host.find(':', start);
    const auto length = (end == std::string_view::npos ? host.size() : end) - start;
    if (length != 0u) {
      if (length > 4u) return false;
      for (std::size_t index = start; index < start + length; ++index) {
        if (!std::isxdigit(static_cast<unsigned char>(host[index]))) return false;
      }
      ++groups;
    }
    if (end == std::string_view::npos) break;
    start = end + 1u;
  }
  return compression == std::string_view::npos ? groups == 8u : groups < 8u;
}

bool valid_url_path(std::string_view path) {
  if (path.empty()) return true;
  if (path.front() != '/' || path.starts_with("//") || path.find('\\') != std::string_view::npos) {
    return false;
  }
  std::string segment;
  for (std::size_t index = 1; index <= path.size(); ++index) {
    if (index == path.size() || path[index] == '/') {
      if (segment == "." || segment == "..") return false;
      segment.clear();
      continue;
    }
    auto c = static_cast<unsigned char>(path[index]);
    if (c < 0x21u || c > 0x7Eu || c == '?' || c == '#') return false;
    if (c == '%') {
      if (index + 2u >= path.size()) return false;
      const int high = hex_value(path[index + 1u]);
      const int low = hex_value(path[index + 2u]);
      if (high < 0 || low < 0) return false;
      c = static_cast<unsigned char>((high << 4) | low);
      if (c < 0x21u || c == 0x7Fu || c == '/' || c == '\\' || c == '?' || c == '#' ||
          c == '@') {
        return false;
      }
      index += 2u;
    }
    segment.push_back(static_cast<char>(std::tolower(c)));
  }
  return true;
}

}  // namespace

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool parse_endpoint(std::string_view url, ParsedEndpoint& parsed) {
  if (url.empty() || url.size() > 2048u || url.find_first_of("?#@\\") != std::string_view::npos) {
    return false;
  }
  for (const unsigned char c : url) {
    if (c < 0x21u || c > 0x7Eu) return false;
  }
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos || scheme_end == 0u) return false;
  parsed.scheme = lowercase(std::string(url.substr(0, scheme_end)));
  if (parsed.scheme != "https" && parsed.scheme != "http" && parsed.scheme != "wss" &&
      parsed.scheme != "ws") {
    return false;
  }
  const auto authority_start = scheme_end + 3u;
  const auto authority_end = url.find('/', authority_start);
  const auto authority = url.substr(
      authority_start, (authority_end == std::string_view::npos ? url.size() : authority_end) -
                           authority_start);
  if (authority.empty()) return false;

  std::string_view host;
  std::string_view port;
  bool port_present = false;
  if (authority.front() == '[') {
    const auto close = authority.find(']');
    if (close == std::string_view::npos || close == 1u) return false;
    host = authority.substr(1u, close - 1u);
    if (!valid_ipv6_literal(host)) return false;
    const auto suffix = authority.substr(close + 1u);
    if (!suffix.empty()) {
      if (suffix.front() != ':') return false;
      port_present = true;
      port = suffix.substr(1u);
    }
  } else {
    const auto colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
      if (authority.find(':') != colon) return false;
      host = authority.substr(0u, colon);
      port_present = true;
      port = authority.substr(colon + 1u);
    } else {
      host = authority;
    }
    if (!valid_dns_host(host)) return false;
  }
  parsed.port = (parsed.scheme == "https" || parsed.scheme == "wss") ? 443u : 80u;
  if (port_present && !parse_port(port, parsed.port)) return false;
  parsed.host = lowercase(std::string(host));
  return valid_url_path(authority_end == std::string_view::npos ? std::string_view{}
                                                               : url.substr(authority_end));
}

bool is_loopback_host(const ParsedEndpoint& endpoint) {
  return endpoint.host == "localhost" || endpoint.host == "127.0.0.1" ||
         endpoint.host == "::1";
}

bool secure_discovered_endpoint(std::string_view url, NetworkEnvironment environment,
                                bool realtime) {
  if (url.empty()) return true;
  ParsedEndpoint endpoint{};
  if (!parse_endpoint(url, endpoint)) return false;
  const bool secure = realtime ? endpoint.scheme == "wss" : endpoint.scheme == "https";
  if (secure) return true;
  const bool local_plaintext = realtime ? endpoint.scheme == "ws" : endpoint.scheme == "http";
  return environment == NetworkEnvironment::Development && local_plaintext &&
         is_loopback_host(endpoint);
}

bool authorized_discovered_endpoint(std::string_view discovered,
                                    std::string_view configured,
                                    std::string_view base_url,
                                    NetworkEnvironment environment,
                                    bool realtime) {
  if (!secure_discovered_endpoint(discovered, environment, realtime)) return false;
  if (discovered.empty()) return true;
  ParsedEndpoint discovered_endpoint{};
  ParsedEndpoint trusted_endpoint{};
  if (!parse_endpoint(discovered, discovered_endpoint) ||
      !parse_endpoint(configured.empty() ? base_url : configured, trusted_endpoint)) {
    return false;
  }
  return discovered_endpoint.host == trusted_endpoint.host &&
         discovered_endpoint.port == trusted_endpoint.port;
}

}  // namespace xenon::network::client_detail
