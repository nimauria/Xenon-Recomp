#include "xenon/core/guest_profiler.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace xenon::core {

void GuestProfiler::record(std::uint64_t elapsed_ms, std::uint32_t thread_id,
                           std::string_view thread_name, std::uint32_t cia,
                           std::string_view export_name, std::uint32_t caller_lr) {
  auto& bucket = buckets_[{elapsed_ms / 1000u, thread_id}];
  if (bucket.thread_name.empty() && !thread_name.empty()) bucket.thread_name = thread_name;
  std::string location;
  if (export_name.empty()) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08X", cia);
    location = text;
  } else {
    location = std::string(export_name);
    if (caller_lr) {
      auto& per_site = blocked_callers_[{bucket.thread_name.empty()
                                             ? "t" + std::to_string(thread_id)
                                             : bucket.thread_name + " t" + std::to_string(thread_id),
                                         std::string(export_name)}];
      ++per_site[caller_lr];
    }
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

void GuestProfiler::write_blocked_callers(std::ostream& out, std::size_t top) const {
  write_blocked_callers(out, top, {});
}

void GuestProfiler::write_blocked_callers(
    std::ostream& out, std::size_t top,
    const std::function<std::string(std::uint32_t)>& name_of) const {
  struct Row {
    const std::pair<std::string, std::string>* key;
    std::uint64_t total;
  };
  std::vector<Row> rows;
  for (const auto& [key, sites] : blocked_callers_) {
    std::uint64_t total = 0;
    for (const auto& [lr, count] : sites) total += count;
    rows.push_back({&key, total});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    return a.total != b.total ? a.total > b.total : *a.key < *b.key;
  });
  for (const auto& row : rows) {
    out << "== " << row.key->first << " blocked in " << row.key->second << ": " << row.total
        << " samples\n";
    const auto& sites = blocked_callers_.at(*row.key);
    std::vector<std::pair<std::uint32_t, std::uint64_t>> ranked(sites.begin(), sites.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
      return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    if (ranked.size() > top) ranked.resize(top);
    for (const auto& [lr, count] : ranked) {
      char line[96];
      std::snprintf(line, sizeof(line), "  %7llu %5.1f%%  lr=%08X",
                    static_cast<unsigned long long>(count),
                    100.0 * static_cast<double>(count) / static_cast<double>(row.total), lr);
      out << line;
      if (name_of) {
        const auto name = name_of(lr);
        if (!name.empty()) out << " in " << name;
      }
      out << '\n';
    }
  }
}

}  // namespace xenon::core
