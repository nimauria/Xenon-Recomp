#include "xenon/core/guest_profiler.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace xenon::core {

void GuestProfiler::record(std::uint64_t elapsed_ms, std::uint32_t thread_id,
                           std::string_view thread_name, std::uint32_t cia,
                           std::string_view export_name) {
  auto& bucket = buckets_[{elapsed_ms / 1000u, thread_id}];
  if (bucket.thread_name.empty() && !thread_name.empty()) bucket.thread_name = thread_name;
  std::string location;
  if (export_name.empty()) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08X", cia);
    location = text;
  } else {
    location = std::string(export_name);
  }
  ++bucket.locations[location];
  ++bucket.total;
  ++samples_;
}

void GuestProfiler::write(std::ostream& out, std::size_t top) const {
  for (const auto& [key, bucket] : buckets_) {
    out << "== " << key.first << "s t" << key.second;
    if (!bucket.thread_name.empty()) out << " (" << bucket.thread_name << ')';
    out << ": " << bucket.total << " samples\n";
    std::vector<std::pair<std::string, std::uint64_t>> ranked(bucket.locations.begin(),
                                                              bucket.locations.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
      return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    if (ranked.size() > top) ranked.resize(top);
    for (const auto& [location, count] : ranked) {
      char line[96];
      std::snprintf(line, sizeof(line), "  %7llu %5.1f%%  ", static_cast<unsigned long long>(count),
                    100.0 * static_cast<double>(count) / static_cast<double>(bucket.total));
      out << line << location << '\n';
    }
  }
}

}  // namespace xenon::core
