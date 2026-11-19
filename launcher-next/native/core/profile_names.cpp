#include "profile_names.hpp"
#include "unicode/profile_unicode.hpp"
#include <algorithm>
#include <stdexcept>
#include <vector>
namespace xenon::launcher_next {
namespace {
struct Scalar {
  std::uint32_t point;
  std::size_t start, end;
};
std::vector<Scalar> decode(std::string_view name) {
  std::vector<Scalar> result;
  for (std::size_t i = 0; i < name.size();) {
    const auto start = i;
    const auto first = static_cast<unsigned char>(name[i++]);
    unsigned tail = 0;
    std::uint32_t point = first, minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      tail = 1;
      point = first & 31;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      tail = 2;
      point = first & 15;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      tail = 3;
      point = first & 7;
      minimum = 0x10000;
    } else if (first >= 0x80)
      throw std::runtime_error("Profile names must contain valid Unicode");
    for (unsigned j = 0; j < tail; j++) {
      if (i == name.size() || (static_cast<unsigned char>(name[i]) & 0xc0) != 0x80)
        throw std::runtime_error("Profile names must contain valid Unicode");
      point = (point << 6) | (static_cast<unsigned char>(name[i++]) & 63);
    }
    if (point < minimum || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff) || point == 0)
      throw std::runtime_error("Profile names must contain valid Unicode without NUL characters");
    result.push_back({point, start, i});
  }
  return result;
}
bool space(std::uint32_t point) {
  return std::binary_search(std::begin(unicode_data::space), std::end(unicode_data::space), point);
}
} // namespace
std::string trim_profile_name(std::string_view name) {
  const auto points = decode(name);
  std::size_t first = 0, last = points.size();
  while (first < last && space(points[first].point))
    ++first;
  while (last > first && space(points[last - 1].point))
    --last;
  return first == last ? std::string{}
                       : std::string(name.substr(points[first].start,
                                                 points[last - 1].end - points[first].start));
}
std::string profile_name_key(std::string_view name) {
  const auto trimmed = trim_profile_name(name);
  std::string result;
  for (const auto &point : decode(trimmed)) {
    const auto found = std::lower_bound(
        std::begin(unicode_data::lower), std::end(unicode_data::lower), point.point,
        [](const auto &entry, auto value) { return entry.point < value; });
    if (found != std::end(unicode_data::lower) && found->point == point.point)
      result += found->value;
    else
      result.append(trimmed, point.start, point.end - point.start);
  }
  return result;
}
} // namespace xenon::launcher_next
