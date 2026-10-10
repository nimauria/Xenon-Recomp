#include "recomp/driver/driver_support.hpp"

#include <cctype>
#include <iomanip>
#include <sstream>

namespace xenon::recomp::detail {

std::string hash_name(std::uint64_t hash) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << hash;
  return out.str();
}

std::string hex_string(std::uint32_t value) {
  std::ostringstream out;
  out << std::hex << value;
  return out.str();
}

std::string json_escape(const std::string& value) {
  std::string escaped;
  for (const char character : value) {
    if (character == '"' || character == '\\') escaped.push_back('\\');
    escaped.push_back(character);
  }
  return escaped;
}

std::string cpp_name(std::uint32_t address) {
  std::ostringstream out;
  out << "xenon_fn_" << std::hex << std::uppercase << address;
  return out.str();
}

std::string sanitize_cpp_name(std::string name, std::uint32_t address) {
  for (auto& character : name)
    if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_')
      character = '_';
  if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front())))
    name = "xenon_" + name;
  return name.empty() ? cpp_name(address) : name;
}

}  // namespace xenon::recomp::detail
