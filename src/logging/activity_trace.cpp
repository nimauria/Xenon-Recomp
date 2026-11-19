#include "xenon/logging/activity_trace.hpp"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

#include "xenon/logging/probe_log.hpp"

namespace xenon::logging {
namespace {

bool enabled_from_environment() {
  const char* value = std::getenv("XENON_ACTIVITY_TRACE");
  return value != nullptr && *value != '\0' && std::string_view(value) != "0";
}

std::atomic<bool> g_enabled{enabled_from_environment()};

std::chrono::steady_clock::time_point trace_epoch() {
  static const auto epoch = std::chrono::steady_clock::now();
  return epoch;
}

// Event kinds. The table never reallocates (kMaxKinds is reserved up front),
// so a key handed out earlier can be read without the lock.
constexpr std::size_t kMaxKinds = 1u << 16;
std::mutex g_kinds_mutex;
std::vector<std::unique_ptr<ActivityKind>>& kinds() {
  static std::vector<std::unique_ptr<ActivityKind>> table = [] {
    std::vector<std::unique_ptr<ActivityKind>> v;
    v.reserve(kMaxKinds);
    // Key 0: an unregistered event.
    v.push_back(std::make_unique<ActivityKind>(ActivityKind{"?", "unknown", {}, false}));
    return v;
  }();
  return table;
}
std::unordered_map<std::string, std::uint32_t>& kind_index() {
  static std::unordered_map<std::string, std::uint32_t> index;
  return index;
}

// The trace file. Entries arrive from every thread's condenser.
constexpr std::uint64_t kTraceByteLimit = std::uint64_t{2} << 30;
struct Writer {
  std::mutex mutex;
  std::FILE* file{};
  std::string path;
  std::uint64_t written{};
  bool truncated{};
  std::chrono::steady_clock::time_point last_flush{};

  void write(const std::string& line) {
    std::scoped_lock lock(mutex);
    const auto directory = probe_log_directory();
    const auto wanted =
        (directory.empty() ? std::filesystem::path("activity_trace.log")
                           : std::filesystem::path(directory) / "activity_trace.log")
            .string();
    if (!file || wanted != path) {
      if (file) std::fclose(file);
      path = wanted;
      std::error_code ignored;
      if (!directory.empty()) std::filesystem::create_directories(directory, ignored);
      file = std::fopen(path.c_str(), "wb");
      written = 0;
      truncated = false;
      if (!file) return;
      static char buffer[1 << 20];
      std::setvbuf(file, buffer, _IOFBF, sizeof(buffer));
    }
    if (truncated) return;
    if (written + line.size() + 1 > kTraceByteLimit) {
      std::fputs("[activity trace truncated: size limit reached]\n", file);
      truncated = true;
      std::fflush(file);
      return;
    }
    std::fwrite(line.data(), 1, line.size(), file);
    std::fputc('\n', file);
    written += line.size() + 1;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_flush > std::chrono::seconds(1)) {
      std::fflush(file);
      last_flush = now;
    }
  }
  void flush() {
    std::scoped_lock lock(mutex);
    if (file) std::fflush(file);
  }
};
Writer& writer() {
  static Writer instance;
  return instance;
}

struct KindStats {
  std::uint64_t count{};
  std::uint64_t total_ns{};
  std::uint64_t max_ns{};
  void add(std::uint64_t ns) {
    ++count;
    total_ns += ns;
    max_ns = std::max(max_ns, ns);
  }
};

std::string thread_name() {
  std::ostringstream out;
  out << "host " << std::this_thread::get_id();
  return out.str();
}

// One thread's stream.
struct Stream {
  std::mutex mutex;
  std::string label;
  std::unique_ptr<ActivityCondenser> condenser;
  std::unordered_map<std::uint32_t, KindStats> stats;
  std::uint64_t first_ns{~0ull};
  std::uint64_t last_ns{};
  std::uint64_t traced_ns{};

  explicit Stream(std::string name) : label(std::move(name)) { make_condenser(); }
  void make_condenser() {
    condenser = std::make_unique<ActivityCondenser>(
        label, [](const std::string& line) { writer().write(line); });
  }
};

std::mutex g_streams_mutex;
std::vector<std::shared_ptr<Stream>>& streams() {
  static std::vector<std::shared_ptr<Stream>> all;
  return all;
}

Stream& current_stream() {
  thread_local std::shared_ptr<Stream> stream;
  if (!stream) {
    stream = std::make_shared<Stream>(thread_name());
    std::scoped_lock lock(g_streams_mutex);
    streams().push_back(stream);
  }
  return *stream;
}

// Two events condense together when they are the same kind and carry the same
// text (two opens of different files stay apart).
bool same_activity(const ActivityEvent& a, const ActivityEvent& b) {
  return a.key == b.key && a.text == b.text;
}

std::string hex(std::uint64_t value) {
  char text[24];
  std::snprintf(text, sizeof(text), "0x%" PRIX64, value);
  return text;
}

std::string seconds(std::uint64_t ns) {
  char text[32];
  std::snprintf(text, sizeof(text), "+%.6fs", double(ns) / 1e9);
  return text;
}

void write_summary() {
  std::vector<std::shared_ptr<Stream>> all;
  {
    std::scoped_lock lock(g_streams_mutex);
    all = streams();
  }
  std::map<std::uint32_t, KindStats> totals;
  std::ostringstream threads;
  for (const auto& stream : all) {
    std::scoped_lock lock(stream->mutex);
    if (stream->stats.empty()) continue;
    const auto span = stream->last_ns > stream->first_ns ? stream->last_ns - stream->first_ns : 0;
    threads << "\n== " << stream->label << ": " << seconds(stream->first_ns) << ".."
            << seconds(stream->last_ns) << ", traced " << format_activity_duration(stream->traced_ns)
            << " of " << format_activity_duration(span) << " lifetime\n";
    std::vector<std::pair<std::uint32_t, KindStats>> rows(stream->stats.begin(),
                                                          stream->stats.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second.total_ns > b.second.total_ns; });
    for (std::size_t i = 0; i < rows.size() && i < 15; ++i) {
      const auto& [key, s] = rows[i];
      const auto& kind = activity_kind(key);
      char line[256];
      std::snprintf(line, sizeof(line), "  %10" PRIu64 "x  total %12s  avg %10s  max %10s  %s %s\n",
                    s.count, format_activity_duration(s.total_ns).c_str(),
                    format_activity_duration(s.count ? s.total_ns / s.count : 0).c_str(),
                    format_activity_duration(s.max_ns).c_str(), kind.category.c_str(),
                    kind.name.c_str());
      threads << line;
    }
    for (const auto& [key, s] : stream->stats) {
      auto& total = totals[key];
      total.count += s.count;
      total.total_ns += s.total_ns;
      total.max_ns = std::max(total.max_ns, s.max_ns);
    }
  }
  std::vector<std::pair<std::uint32_t, KindStats>> rows(totals.begin(), totals.end());
  std::ostringstream out;
  const auto table = [&](const char* title, std::size_t limit) {
    out << "\n== " << title << "\n";
    for (std::size_t i = 0; i < rows.size() && i < limit; ++i) {
      const auto& [key, s] = rows[i];
      const auto& kind = activity_kind(key);
      char line[256];
      std::snprintf(line, sizeof(line), "%12" PRIu64 "x  total %12s  avg %10s  max %10s  %s %s\n",
                    s.count, format_activity_duration(s.total_ns).c_str(),
                    format_activity_duration(s.count ? s.total_ns / s.count : 0).c_str(),
                    format_activity_duration(s.max_ns).c_str(), kind.category.c_str(),
                    kind.name.c_str());
      out << line;
    }
  };
  out << "Xenon activity summary at " << seconds(activity_now_ns()) << " (" << rows.size()
      << " event kinds, " << all.size() << " threads)\n";
  std::sort(rows.begin(), rows.end(),
            [](const auto& a, const auto& b) { return a.second.total_ns > b.second.total_ns; });
  table("by total time", 300);
  std::sort(rows.begin(), rows.end(),
            [](const auto& a, const auto& b) { return a.second.count > b.second.count; });
  table("by count", 150);
  std::sort(rows.begin(), rows.end(),
            [](const auto& a, const auto& b) { return a.second.max_ns > b.second.max_ns; });
  table("by longest single event", 60);
  out << "\n== per thread (top 15 each, by total time)" << threads.str();
  const auto directory = probe_log_directory();
  std::error_code ignored;
  if (!directory.empty()) std::filesystem::create_directories(directory, ignored);
  const auto path = directory.empty()
                        ? std::filesystem::path("activity_summary.txt")
                        : std::filesystem::path(directory) / "activity_summary.txt";
  if (std::FILE* file = std::fopen(path.string().c_str(), "wb")) {
    const auto text = out.str();
    std::fwrite(text.data(), 1, text.size(), file);
    std::fclose(file);
  }
}

}  // namespace

bool activity_trace_enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }

void set_activity_trace_enabled(bool enabled) noexcept {
  g_enabled.store(enabled, std::memory_order_relaxed);
}

std::uint32_t intern_activity(const ActivityKind& kind) {
  std::string id = kind.category;
  id += '\0';
  id += kind.name;
  for (const auto& label : kind.labels) {
    id += '\0';
    id += label;
  }
  id += kind.has_result ? '1' : '0';
  std::scoped_lock lock(g_kinds_mutex);
  auto& table = kinds();
  auto& index = kind_index();
  if (const auto found = index.find(id); found != index.end()) return found->second;
  if (table.size() >= kMaxKinds) return 0;
  const auto key = static_cast<std::uint32_t>(table.size());
  table.push_back(std::make_unique<ActivityKind>(kind));
  index.emplace(std::move(id), key);
  return key;
}

const ActivityKind& activity_kind(std::uint32_t key) {
  auto& table = kinds();
  return key < table.size() ? *table[key] : *table[0];
}

std::uint64_t activity_now_ns() noexcept {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - trace_epoch())
                                        .count());
}

void record_activity(std::uint32_t key, const std::array<std::uint64_t, 4>& arguments,
                     std::uint64_t result, std::uint64_t start_ns, std::uint64_t duration_ns,
                     std::string text) {
  if (!activity_trace_enabled()) return;
  auto& stream = current_stream();
  std::scoped_lock lock(stream.mutex);
  stream.stats[key].add(duration_ns);
  stream.first_ns = std::min(stream.first_ns, start_ns);
  stream.last_ns = std::max(stream.last_ns, start_ns + duration_ns);
  stream.traced_ns += duration_ns;
  stream.condenser->add(ActivityEvent{key, arguments, result, start_ns, duration_ns,
                                      std::move(text)});
}

void set_activity_thread_label(std::string label) {
  if (!activity_trace_enabled()) return;
  auto& stream = current_stream();
  std::scoped_lock lock(stream.mutex);
  if (stream.label == label) return;
  stream.condenser->flush();
  stream.label = std::move(label);
  stream.make_condenser();
}

ActivityScope::ActivityScope(std::uint32_t key, std::array<std::uint64_t, 4> args) noexcept
    : arguments(args), key_(key), active_(activity_trace_enabled()) {
  if (active_) start_ = activity_now_ns();
}

ActivityScope::~ActivityScope() {
  if (!active_) return;
  const auto end = activity_now_ns();
  record_activity(key_, arguments, result, start_, end - start_, std::move(text));
}

void flush_activity_trace() {
  std::vector<std::shared_ptr<Stream>> all;
  {
    std::scoped_lock lock(g_streams_mutex);
    all = streams();
  }
  for (const auto& stream : all) {
    std::scoped_lock lock(stream->mutex);
    stream->condenser->flush();
  }
  writer().flush();
  write_summary();
}

void write_activity_summary() {
  if (!activity_trace_enabled()) return;
  writer().flush();
  write_summary();
}

std::string format_activity_duration(std::uint64_t ns) {
  char text[32];
  if (ns >= 100'000)
    std::snprintf(text, sizeof(text), "%.2f ms", double(ns) / 1e6);
  else
    std::snprintf(text, sizeof(text), "%.1f us", double(ns) / 1e3);
  return text;
}

ActivityCondenser::ActivityCondenser(std::string label,
                                     std::function<void(const std::string&)> sink)
    : label_(std::move(label)), sink_(std::move(sink)) {}

std::string ActivityCondenser::describe(const ActivityEvent& event) const {
  const auto& kind = activity_kind(event.key);
  std::string out = kind.category + ' ' + kind.name;
  bool any = false;
  for (std::size_t i = 0; i < kind.labels.size(); ++i) {
    if (kind.labels[i].empty()) continue;
    out += any ? ' ' : '(';
    out += kind.labels[i] + '=' + hex(event.arguments[i]);
    any = true;
  }
  if (any) out += ')';
  if (kind.has_result) out += " -> " + hex(event.result);
  if (!event.text.empty()) out += " \"" + event.text + '"';
  return out;
}

void ActivityCondenser::emit_single(const ActivityEvent& event) {
  sink_(seconds(event.start_ns) + ' ' + label_ + "  " + describe(event) + "  " +
        format_activity_duration(event.duration_ns));
}

void ActivityCondenser::close_cycle(bool continues) {
  if (cycle_.empty()) return;
  std::string head = seconds(cycle_start_ns_) + ".." + seconds(cycle_end_ns_) + ' ' + label_ +
                     "  x" + std::to_string(repeats_);
  if (position_) head += " (+" + std::to_string(position_) + " steps)";
  if (continued_) head += " [continued]";
  if (cycle_.size() == 1) {
    const auto& step = cycle_.front();
    sink_(head + "  " + describe(step.first) + "  avg " +
          format_activity_duration(step.count ? step.total_ns / step.count : 0) + ", max " +
          format_activity_duration(step.max_ns) + (continues ? "  (continues)" : ""));
  } else {
    std::uint64_t per_cycle = 0;
    for (const auto& step : cycle_) per_cycle += step.count ? step.total_ns / step.count : 0;
    sink_(head + " cycle of " + std::to_string(cycle_.size()) + ", " +
          format_activity_duration(per_cycle) + "/cycle" + (continues ? "  (continues)" : "") +
          ':');
    for (const auto& step : cycle_)
      sink_("    | " + describe(step.first) + "  avg " +
            format_activity_duration(step.count ? step.total_ns / step.count : 0) + ", max " +
            format_activity_duration(step.max_ns));
  }
  if (continues) {
    for (auto& step : cycle_) step.count = step.total_ns = step.max_ns = 0;
    repeats_ = 0;
    cycle_start_ns_ = cycle_end_ns_;
    cycle_emitted_ns_ = cycle_end_ns_;
    continued_ = true;
  } else {
    cycle_.clear();
    position_ = 0;
    repeats_ = 0;
    continued_ = false;
  }
}

void ActivityCondenser::add(ActivityEvent event) {
  if (!cycle_.empty()) {
    auto& step = cycle_[position_];
    if (same_activity(event, step.first)) {
      ++step.count;
      step.total_ns += event.duration_ns;
      step.max_ns = std::max(step.max_ns, event.duration_ns);
      cycle_end_ns_ = event.start_ns + event.duration_ns;
      if (++position_ == cycle_.size()) {
        position_ = 0;
        ++repeats_;
        if (cycle_end_ns_ - cycle_emitted_ns_ >= kProgressNs) close_cycle(true);
      }
      return;
    }
    close_cycle(false);
  }
  pending_.push_back(std::move(event));
  // The newest events as back-to-back copies of their last k start a cycle
  // of k: two copies for a cycle of several steps, three for one event
  // repeating (so "A A B A A B" is seen as a cycle of three rather than as
  // "A" twice). The longest cycle wins.
  const auto size = pending_.size();
  for (std::size_t k = kMaxCycle; k >= 1; --k) {
    const std::size_t copies = k == 1 ? 3 : 2;
    if (copies * k > size) continue;
    bool repeats = true;
    for (std::size_t c = 1; c < copies && repeats; ++c)
      for (std::size_t i = 0; i < k && repeats; ++i)
        repeats = same_activity(pending_[size - k + i], pending_[size - (c + 1) * k + i]);
    if (!repeats) continue;
    const auto begin = size - copies * k;
    for (std::size_t i = 0; i < begin; ++i) emit_single(pending_[i]);
    cycle_.assign(k, Step{});
    for (std::size_t i = 0; i < k; ++i) cycle_[i].first = pending_[begin + i];
    for (std::size_t j = begin; j < size; ++j) {
      auto& step = cycle_[(j - begin) % k];
      ++step.count;
      step.total_ns += pending_[j].duration_ns;
      step.max_ns = std::max(step.max_ns, pending_[j].duration_ns);
    }
    cycle_start_ns_ = pending_[begin].start_ns;
    cycle_end_ns_ = pending_.back().start_ns + pending_.back().duration_ns;
    cycle_emitted_ns_ = cycle_start_ns_;
    position_ = 0;
    repeats_ = copies;
    continued_ = false;
    pending_.clear();
    return;
  }
  if (pending_.size() > kWindow) {
    emit_single(pending_.front());
    pending_.erase(pending_.begin());
  }
}

void ActivityCondenser::flush() {
  close_cycle(false);
  for (const auto& event : pending_) emit_single(event);
  pending_.clear();
}

}  // namespace xenon::logging
