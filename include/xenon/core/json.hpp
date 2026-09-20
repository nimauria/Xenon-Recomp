#pragma once

// Minimal, dependency-free JSON value/parser/writer used for the runtime
// host's process boundary (launch configuration in, status/log out). This
// intentionally stays outside Qt so xenon_core and the runtime host never
// depend on the launcher's toolkit; see docs/architecture/PROJECT_STRUCTURE.md.
// It supports the JSON subset this project needs: objects, arrays, strings,
// numbers, booleans and null. It is not a general-purpose validator.

#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace xenon::core {

class JsonValue {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  using Array = std::vector<JsonValue>;
  using Object = std::map<std::string, JsonValue>;

  JsonValue() : data_(nullptr) {}
  JsonValue(std::nullptr_t) : data_(nullptr) {}
  JsonValue(bool value) : data_(value) {}
  JsonValue(double value) : data_(value) {}
  JsonValue(int value) : data_(static_cast<double>(value)) {}
  JsonValue(std::uint32_t value) : data_(static_cast<double>(value)) {}
  JsonValue(std::int64_t value) : data_(static_cast<double>(value)) {}
  JsonValue(std::uint64_t value) : data_(static_cast<double>(value)) {}
  JsonValue(std::string value) : data_(std::move(value)) {}
  JsonValue(const char* value) : data_(std::string(value)) {}
  JsonValue(Array value) : data_(std::move(value)) {}
  JsonValue(Object value) : data_(std::move(value)) {}

  [[nodiscard]] static JsonValue make_object() { return JsonValue(Object{}); }
  [[nodiscard]] static JsonValue make_array() { return JsonValue(Array{}); }

  [[nodiscard]] Type type() const noexcept { return static_cast<Type>(data_.index()); }
  [[nodiscard]] bool is_null() const noexcept { return type() == Type::Null; }
  [[nodiscard]] bool is_object() const noexcept { return type() == Type::Object; }
  [[nodiscard]] bool is_array() const noexcept { return type() == Type::Array; }
  [[nodiscard]] bool is_string() const noexcept { return type() == Type::String; }

  [[nodiscard]] bool as_bool(bool fallback = false) const {
    return std::holds_alternative<bool>(data_) ? std::get<bool>(data_) : fallback;
  }
  [[nodiscard]] double as_number(double fallback = 0.0) const {
    return std::holds_alternative<double>(data_) ? std::get<double>(data_) : fallback;
  }
  [[nodiscard]] std::uint32_t as_uint32(std::uint32_t fallback = 0) const {
    return std::holds_alternative<double>(data_)
               ? static_cast<std::uint32_t>(std::get<double>(data_))
               : fallback;
  }
  [[nodiscard]] std::int64_t as_int64(std::int64_t fallback = 0) const {
    return std::holds_alternative<double>(data_)
               ? static_cast<std::int64_t>(std::get<double>(data_))
               : fallback;
  }
  [[nodiscard]] std::string as_string(std::string fallback = {}) const {
    return std::holds_alternative<std::string>(data_) ? std::get<std::string>(data_)
                                                       : fallback;
  }
  [[nodiscard]] const Array* as_array() const {
    return std::holds_alternative<Array>(data_) ? &std::get<Array>(data_) : nullptr;
  }
  [[nodiscard]] const Object* as_object() const {
    return std::holds_alternative<Object>(data_) ? &std::get<Object>(data_) : nullptr;
  }

  // Object helpers. set()/append() are no-ops if the value is not the right
  // container type (they lazily become one when default-constructed).
  JsonValue& set(std::string key, JsonValue value) {
    if (!std::holds_alternative<Object>(data_)) data_ = Object{};
    std::get<Object>(data_)[std::move(key)] = std::move(value);
    return *this;
  }
  void append(JsonValue value) {
    if (!std::holds_alternative<Array>(data_)) data_ = Array{};
    std::get<Array>(data_).push_back(std::move(value));
  }

  [[nodiscard]] const JsonValue* find(std::string_view key) const {
    const auto* object = as_object();
    if (!object) return nullptr;
    const auto it = object->find(std::string(key));
    return it == object->end() ? nullptr : &it->second;
  }
  [[nodiscard]] JsonValue get(std::string_view key, JsonValue fallback = {}) const {
    const auto* value = find(key);
    return value ? *value : fallback;
  }
  [[nodiscard]] std::string get_string(std::string_view key, std::string fallback = {}) const {
    const auto* value = find(key);
    return value ? value->as_string(fallback) : fallback;
  }
  [[nodiscard]] bool get_bool(std::string_view key, bool fallback = false) const {
    const auto* value = find(key);
    return value ? value->as_bool(fallback) : fallback;
  }
  [[nodiscard]] double get_number(std::string_view key, double fallback = 0.0) const {
    const auto* value = find(key);
    return value ? value->as_number(fallback) : fallback;
  }

  // Serialization.
  [[nodiscard]] std::string dump(int indent = -1) const {
    std::ostringstream out;
    write(out, indent, 0);
    return out.str();
  }

  // Parsing. Returns false and sets *error on malformed input.
  [[nodiscard]] static bool parse(std::string_view text, JsonValue& out,
                                  std::string* error = nullptr) {
    Parser parser{text};
    parser.skip_ws();
    if (!parser.parse_value(out)) {
      if (error) *error = parser.error.empty() ? "Invalid JSON" : parser.error;
      return false;
    }
    parser.skip_ws();
    if (!parser.at_end()) {
      if (error) *error = "Trailing content after JSON value";
      return false;
    }
    return true;
  }

 private:
  std::variant<std::nullptr_t, bool, double, std::string, Array, Object> data_;

  static void write_escaped(std::ostream& out, const std::string& value) {
    out << '"';
    for (unsigned char c : value) {
      switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
          if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
            out << buf;
          } else {
            out << static_cast<char>(c);
          }
      }
    }
    out << '"';
  }

  void write(std::ostream& out, int indent, int depth) const {
    const auto pad = [&](int d) {
      if (indent >= 0) out << '\n' << std::string(static_cast<std::size_t>(indent * d), ' ');
    };
    switch (type()) {
      case Type::Null: out << "null"; break;
      case Type::Bool: out << (std::get<bool>(data_) ? "true" : "false"); break;
      case Type::Number: {
        const double value = std::get<double>(data_);
        if (value == static_cast<std::int64_t>(value)) {
          out << static_cast<std::int64_t>(value);
        } else {
          out << value;
        }
        break;
      }
      case Type::String: write_escaped(out, std::get<std::string>(data_)); break;
      case Type::Array: {
        const auto& array = std::get<Array>(data_);
        out << '[';
        bool first = true;
        for (const auto& entry : array) {
          if (!first) out << ',';
          first = false;
          pad(depth + 1);
          entry.write(out, indent, depth + 1);
        }
        if (!array.empty()) pad(depth);
        out << ']';
        break;
      }
      case Type::Object: {
        const auto& object = std::get<Object>(data_);
        out << '{';
        bool first = true;
        for (const auto& [key, value] : object) {
          if (!first) out << ',';
          first = false;
          pad(depth + 1);
          write_escaped(out, key);
          out << ':';
          if (indent >= 0) out << ' ';
          value.write(out, indent, depth + 1);
        }
        if (!object.empty()) pad(depth);
        out << '}';
        break;
      }
    }
  }

  struct Parser {
    std::string_view text;
    std::size_t pos{0};
    std::string error{};

    [[nodiscard]] bool at_end() const noexcept { return pos >= text.size(); }
    void skip_ws() {
      while (pos < text.size() &&
             (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r')) {
        ++pos;
      }
    }
    [[nodiscard]] bool consume(char expected) {
      if (pos < text.size() && text[pos] == expected) {
        ++pos;
        return true;
      }
      return false;
    }
    [[nodiscard]] bool literal(std::string_view word) {
      if (text.substr(pos, word.size()) == word) {
        pos += word.size();
        return true;
      }
      return false;
    }

    bool parse_value(JsonValue& out) {
      skip_ws();
      if (at_end()) { error = "Unexpected end of input"; return false; }
      const char c = text[pos];
      if (c == '{') return parse_object(out);
      if (c == '[') return parse_array(out);
      if (c == '"') { std::string s; if (!parse_string(s)) return false; out = JsonValue(std::move(s)); return true; }
      if (literal("true")) { out = JsonValue(true); return true; }
      if (literal("false")) { out = JsonValue(false); return true; }
      if (literal("null")) { out = JsonValue(nullptr); return true; }
      if (c == '-' || (c >= '0' && c <= '9')) return parse_number(out);
      error = "Unexpected character in JSON";
      return false;
    }

    bool parse_number(JsonValue& out) {
      const std::size_t start = pos;
      if (pos < text.size() && text[pos] == '-') ++pos;
      while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
      if (pos < text.size() && text[pos] == '.') {
        ++pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
      }
      if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
        ++pos;
        if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) ++pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
      }
      if (pos == start) { error = "Invalid number"; return false; }
      try {
        out = JsonValue(std::stod(std::string(text.substr(start, pos - start))));
      } catch (...) {
        error = "Invalid number";
        return false;
      }
      return true;
    }

    bool parse_string(std::string& out) {
      if (!consume('"')) { error = "Expected string"; return false; }
      out.clear();
      while (pos < text.size() && text[pos] != '"') {
        char c = text[pos++];
        if (c == '\\' && pos < text.size()) {
          const char escape = text[pos++];
          switch (escape) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
              if (pos + 4 > text.size()) { error = "Invalid unicode escape"; return false; }
              unsigned int code = 0;
              for (int i = 0; i < 4; ++i) {
                code <<= 4;
                const char hex = text[pos++];
                if (hex >= '0' && hex <= '9') code |= static_cast<unsigned int>(hex - '0');
                else if (hex >= 'a' && hex <= 'f') code |= static_cast<unsigned int>(hex - 'a' + 10);
                else if (hex >= 'A' && hex <= 'F') code |= static_cast<unsigned int>(hex - 'A' + 10);
                else { error = "Invalid unicode escape"; return false; }
              }
              // Basic BMP-only conversion to UTF-8 (sufficient for paths/text
              // used in this project's launch/status contracts).
              if (code < 0x80) {
                out += static_cast<char>(code);
              } else if (code < 0x800) {
                out += static_cast<char>(0xC0 | (code >> 6));
                out += static_cast<char>(0x80 | (code & 0x3F));
              } else {
                out += static_cast<char>(0xE0 | (code >> 12));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
              }
              break;
            }
            default: out += escape;
          }
        } else {
          out += c;
        }
      }
      if (!consume('"')) { error = "Unterminated string"; return false; }
      return true;
    }

    bool parse_array(JsonValue& out) {
      if (!consume('[')) return false;
      JsonValue::Array array;
      skip_ws();
      if (consume(']')) { out = JsonValue(std::move(array)); return true; }
      while (true) {
        JsonValue element;
        if (!parse_value(element)) return false;
        array.push_back(std::move(element));
        skip_ws();
        if (consume(',')) { skip_ws(); continue; }
        if (consume(']')) break;
        error = "Expected ',' or ']' in array";
        return false;
      }
      out = JsonValue(std::move(array));
      return true;
    }

    bool parse_object(JsonValue& out) {
      if (!consume('{')) return false;
      JsonValue::Object object;
      skip_ws();
      if (consume('}')) { out = JsonValue(std::move(object)); return true; }
      while (true) {
        skip_ws();
        std::string key;
        if (!parse_string(key)) return false;
        skip_ws();
        if (!consume(':')) { error = "Expected ':' in object"; return false; }
        JsonValue value;
        if (!parse_value(value)) return false;
        object[std::move(key)] = std::move(value);
        skip_ws();
        if (consume(',')) continue;
        if (consume('}')) break;
        error = "Expected ',' or '}' in object";
        return false;
      }
      out = JsonValue(std::move(object));
      return true;
    }
  };
};

}  // namespace xenon::core
