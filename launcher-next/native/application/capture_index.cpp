#include "capture_index.hpp"
#include "core/state_file.hpp"
#include <algorithm>
#include <chrono>
#include <queue>

namespace xenon::launcher_next {
namespace fs = std::filesystem;
using Json = nlohmann::json;
namespace {
std::string utf8(const fs::path &path) {
  const auto value = path.u8string();
  return {value.begin(), value.end()};
}
struct Capture {
  std::string name;
  std::uintmax_t size;
  std::int64_t modified;
};
bool newer(const Capture &a, const Capture &b) {
  return a.modified != b.modified ? a.modified > b.modified : a.name < b.name;
}
} // namespace
bool capture_extension(const fs::path &file) {
  auto ext = utf8(file.extension());
  for (auto &c : ext)
    if (c >= 'A' && c <= 'Z')
      c += 'a' - 'A';
  return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".webp" || ext == ".bmp";
}
CaptureIndex::CaptureIndex() : worker_([this] { run(); }) {}
CaptureIndex::~CaptureIndex() {
  {
    std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  changed_.notify_all();
  worker_.join();
}
void CaptureIndex::folder(const fs::path &root) {
  std::lock_guard lock(mutex_);
  if (root_ == root)
    return;
  root_ = root;
  ++generation_;
  result_ = {{"state", "scanning"}, {"entries", Json::array()}, {"truncated", false}};
  requested_ = true;
  changed_.notify_one();
}
void CaptureIndex::refresh() {
  std::lock_guard lock(mutex_);
  requested_ = true;
  result_["state"] = "scanning";
  changed_.notify_one();
}
Json CaptureIndex::snapshot() const {
  std::lock_guard lock(mutex_);
  return result_;
}
void CaptureIndex::run() noexcept {
  fs::path previous;
  fs::file_time_type last_stamp{};
  for (;;) {
    fs::path root;
    std::uint64_t generation;
    bool requested;
    {
      std::unique_lock lock(mutex_);
      changed_.wait_for(lock, std::chrono::seconds(1), [&] { return stopping_ || requested_; });
      if (stopping_)
        return;
      root = root_;
      generation = generation_;
      requested = requested_;
      requested_ = false;
    }
    if (root.empty())
      continue;
    try {
      std::error_code error;
      const auto stamp = fs::last_write_time(root, error);
      if (!requested && previous == root && stamp == last_stamp)
        continue;
      previous = root;
      last_stamp = stamp;
      // Keep only the newest 10,000 entries even when the folder is much larger.
      std::priority_queue<Capture, std::vector<Capture>, decltype(&newer)> entries(newer);
      std::size_t visited = 0, images = 0;
      bool bounded = false;
      if (fs::exists(root)) {
        fs::directory_iterator iterator(root, error), end;
        if (error)
          throw std::runtime_error("The screenshots folder could not be read");
        while (iterator != end) {
          if (++visited > 100000) {
            bounded = true;
            break;
          }
          if (visited % 128 == 0) {
            std::lock_guard lock(mutex_);
            if (stopping_)
              return;
            if (generation != generation_)
              break;
          }
          const auto &entry = *iterator;
          if (capture_extension(entry.path()) && !entry.is_symlink(error) &&
              entry.is_regular_file(error)) {
            const auto size = entry.file_size(error);
            if (!error) {
              const auto time = entry.last_write_time(error);
              if (!error) {
#ifdef _MSC_VER
                const auto system_time = std::chrono::clock_cast<std::chrono::system_clock>(time);
#else
                const auto system_time = fs::file_time_type::clock::to_sys(time);
#endif
                const auto modified = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          system_time.time_since_epoch())
                                          .count();
                Capture capture{utf8(entry.path().filename()), size, modified};
                ++images;
                if (entries.size() < 10000)
                  entries.push(std::move(capture));
                else if (newer(capture, entries.top())) {
                  entries.pop();
                  entries.push(std::move(capture));
                }
              }
            }
          }
          error.clear();
          iterator.increment(error);
          if (error)
            throw std::runtime_error(
                "The screenshots folder changed or could not be read; refresh to retry");
        }
      }
      std::vector<Capture> ordered;
      while (!entries.empty()) {
        ordered.push_back(entries.top());
        entries.pop();
      }
      std::reverse(ordered.begin(), ordered.end());
      Json rows = Json::array();
      for (const auto &entry : ordered)
        rows.push_back(
            {{"id", entry.name},
             {"name", entry.name},
             {"size", entry.size},
             {"modified", entry.modified},
             {"version", std::to_string(entry.modified) + "-" + std::to_string(entry.size)},
             {"viewable", entry.size <= 20 * 1024 * 1024}});
      std::lock_guard lock(mutex_);
      if (generation == generation_)
        result_ = {{"state", "ready"}, {"entries", rows}, {"truncated", bounded || images > 10000}};
    } catch (const std::exception &error) {
      std::lock_guard lock(mutex_);
      if (generation == generation_) {
        result_["state"] = "failed";
        result_["error"] = error.what();
      }
    }
  }
}
} // namespace xenon::launcher_next
