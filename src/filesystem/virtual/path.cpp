#include "xenon/filesystem/path.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

namespace xenon::filesystem {
namespace {

[[nodiscard]] char ascii_lower(char value) noexcept {
  if (value >= 'A' && value <= 'Z') {
    return static_cast<char>(value - 'A' + 'a');
  }
  return value;
}

[[nodiscard]] bool contains_nul(std::string_view value) noexcept {
  return value.find('\0') != std::string_view::npos;
}

}  // namespace

std::string_view to_string(FsError error) noexcept {
  switch (error) {
    case FsError::None:
      return "none";
    case FsError::InvalidPath:
      return "invalid_path";
    case FsError::NotFound:
      return "not_found";
    case FsError::AlreadyExists:
      return "already_exists";
    case FsError::AccessDenied:
      return "access_denied";
    case FsError::ReadOnly:
      return "read_only";
    case FsError::NotDirectory:
      return "not_directory";
    case FsError::IsDirectory:
      return "is_directory";
    case FsError::DirectoryNotEmpty:
      return "directory_not_empty";
    case FsError::InvalidArgument:
      return "invalid_argument";
    case FsError::IoError:
      return "io_error";
    case FsError::Unsupported:
      return "unsupported";
    case FsError::CrossDevice:
      return "cross_device";
    case FsError::TooManyLinks:
      return "too_many_links";
    case FsError::SharingViolation:
      return "sharing_violation";
  }
  return "unknown";
}

FsError normalize_guest_path(std::string_view input, std::string& output,
                             bool allow_relative) {
  output.clear();
  if (input.empty() || contains_nul(input)) return FsError::InvalidPath;

  std::string path(input);
  std::replace(path.begin(), path.end(), '/', '\\');

  const bool rooted = !path.empty() && path.front() == '\\';
  const auto colon = path.find(':');
  const auto first_separator = path.find('\\');
  const bool has_alias =
      colon != std::string::npos &&
      (first_separator == std::string::npos || colon < first_separator);

  if (!rooted && !has_alias && !allow_relative) return FsError::InvalidPath;
  if (has_alias && colon == 0) return FsError::InvalidPath;

  std::string prefix;
  std::size_t cursor = 0;
  if (rooted) {
    prefix = "\\";
    cursor = 1;
  } else if (has_alias) {
    prefix.assign(path.data(), colon + 1);
    cursor = colon + 1;
    while (cursor < path.size() && path[cursor] == '\\') ++cursor;
  }

  std::vector<std::string> components;
  while (cursor <= path.size()) {
    const auto next = path.find('\\', cursor);
    const auto end = next == std::string::npos ? path.size() : next;
    const auto component = std::string_view(path).substr(cursor, end - cursor);

    if (!component.empty() && component != ".") {
      if (component == "..") {
        if (components.empty()) return FsError::InvalidPath;
        components.pop_back();
      } else {
        if (component.find(':') != std::string_view::npos) {
          return FsError::InvalidPath;
        }
        components.emplace_back(component);
      }
    }

    if (next == std::string::npos) break;
    cursor = next + 1;
  }

  output = prefix;
  if (has_alias && !components.empty()) output.push_back('\\');
  for (std::size_t i = 0; i < components.size(); ++i) {
    if (!output.empty() && output.back() != '\\' && output.back() != ':') {
      output.push_back('\\');
    }
    output += components[i];
  }

  if (rooted && output.empty()) output = "\\";
  if (output.empty()) return FsError::InvalidPath;
  return FsError::None;
}

bool guest_path_equal(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) return false;
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    if (ascii_lower(lhs[i]) != ascii_lower(rhs[i])) return false;
  }
  return true;
}

bool guest_path_has_prefix(std::string_view path,
                           std::string_view prefix) noexcept {
  if (prefix.empty() || path.size() < prefix.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (ascii_lower(path[i]) != ascii_lower(prefix[i])) return false;
  }
  return path.size() == prefix.size() || path[prefix.size()] == '\\';
}

bool guest_path_is_absolute(std::string_view path) noexcept {
  if (path.empty()) return false;
  if (path.front() == '\\') return true;
  const auto colon = path.find(':');
  const auto separator = path.find_first_of("\\/");
  return colon != std::string_view::npos &&
         (separator == std::string_view::npos || colon < separator);
}

bool guest_wildcard_match(std::string_view pattern,
                          std::string_view value) noexcept {
  // Xbox directory queries ultimately inherit NT/DOS wildcard behavior. In
  // addition to '*' and '?', NT may pass DOS_STAR ('<'), DOS_QM ('>') and
  // DOS_DOT ('"') tokens after expression translation. Keep the matcher
  // host-neutral so Linux and Windows enumerate the same guest names.
  if (pattern.empty()) pattern = "*";
  if (pattern == "*" || pattern == "*.*") return true;

  const auto match_core = [&](std::string_view expression) noexcept {
    const std::size_t rows = expression.size() + 1;
    const std::size_t cols = value.size() + 1;
    std::vector<std::int8_t> memo(rows * cols, -1);

    const auto recurse = [&](auto&& self, std::size_t p,
                             std::size_t v) noexcept -> bool {
      auto& cached = memo[p * cols + v];
      if (cached != -1) return cached != 0;

      bool matched = false;
      if (p == expression.size()) {
        matched = v == value.size();
      } else {
        const char token = expression[p];
        if (token == '*') {
          std::size_t next = p;
          while (next < expression.size() && expression[next] == '*') ++next;
          if (next == expression.size()) {
            matched = true;
          } else {
            for (std::size_t candidate = v; candidate <= value.size(); ++candidate) {
              if (self(self, next, candidate)) {
                matched = true;
                break;
              }
            }
          }
        } else if (token == '?') {
          matched = v < value.size() && self(self, p + 1, v + 1);
        } else if (token == '<') {  // DOS_STAR
          // DOS_STAR may consume characters up to (but not beyond) the final
          // period in the remaining name. Without a period it behaves as '*'.
          auto final_dot = value.find_last_of('.');
          const std::size_t limit =
              final_dot != std::string_view::npos && final_dot >= v
                  ? final_dot
                  : value.size();
          for (std::size_t candidate = v; candidate <= limit; ++candidate) {
            if (self(self, p + 1, candidate)) {
              matched = true;
              break;
            }
          }
        } else if (token == '>') {  // DOS_QM
          if (v < value.size() && value[v] != '.') {
            matched = self(self, p + 1, v + 1);
          } else {
            // At a dot/end, a run of DOS_QM tokens is allowed to match zero
            // characters (the behavior needed by NT filename expressions).
            std::size_t next = p;
            while (next < expression.size() && expression[next] == '>') ++next;
            matched = self(self, next, v);
          }
        } else if (token == '"') {  // DOS_DOT
          if (v < value.size() && value[v] == '.') {
            matched = self(self, p + 1, v + 1);
          } else if (v == value.size()) {
            matched = self(self, p + 1, v);
          }
        } else {
          matched = v < value.size() &&
                    ascii_lower(token) == ascii_lower(value[v]) &&
                    self(self, p + 1, v + 1);
        }
      }

      cached = matched ? 1 : 0;
      return matched;
    };

    return recurse(recurse, 0, 0);
  };

  if (match_core(pattern)) return true;

  // Win32/DOS-style "name.*" also matches a name with no extension. This is
  // commonly observed in game directory enumeration and is intentionally
  // handled in the generic guest matcher rather than by a host filesystem.
  if (pattern.size() >= 2 && pattern.substr(pattern.size() - 2) == ".*") {
    return match_core(pattern.substr(0, pattern.size() - 2));
  }
  return false;
}

std::string guest_path_key(std::string_view path) {
  std::string key(path);
  std::transform(key.begin(), key.end(), key.begin(), ascii_lower);
  return key;
}

}  // namespace xenon::filesystem
