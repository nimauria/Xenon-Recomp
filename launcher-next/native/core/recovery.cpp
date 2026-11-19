#include "recovery.hpp"
#include "file_transfer.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <regex>
namespace xenon::launcher_next {
namespace fs = std::filesystem;
namespace {
std::string timestamp() {
  return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
}
Json read_state(const fs::path &path) {
  if (!fs::exists(path))
    return Json::object();
  const auto value = read_document(path, 64 * 1024);
  if (!value.is_object())
    throw std::runtime_error("Recovery document must be an object");
  return value;
}
} // namespace
RecoveryState::RecoveryState(fs::path storage, bool explicit_safe_mode)
    : storage_(fs::weakly_canonical(std::move(storage))), run_id_(make_id()) {
  try {
    state_ = read_state(storage_ / "recovery.json");
  } catch (const std::exception &) {
    fs::copy_file(storage_ / "recovery.json", storage_ / ("recovery.invalid-" + run_id_ + ".json"));
    state_ = {{"invalidRecoveryStatePreserved", true}};
  }
  const bool unclean = fs::exists(storage_ / "running.json");
  Json previous = Json::object();
  if (unclean)
    try {
      previous = read_state(storage_ / "running.json");
    } catch (...) {
    }
  unsigned count = 0;
  if (state_.contains("consecutiveUncleanStarts") &&
      state_["consecutiveUncleanStarts"].is_number_unsigned())
    count = static_cast<unsigned>(
        std::min<std::uint64_t>(100000, state_["consecutiveUncleanStarts"].get<std::uint64_t>()));
  count = unclean ? count + 1 : 0;
  const auto phase = previous.contains("phase") && previous["phase"].is_string()
                         ? previous["phase"].get<std::string>()
                         : "unknown";
  const bool startup_failure = unclean && phase != "interface-ready" && phase != "interactive";
  const bool automatic = startup_failure || (unclean && count >= 2);
  state_["previousUncleanExit"] = unclean;
  state_["consecutiveUncleanStarts"] = count;
  state_["safeMode"] = explicit_safe_mode || automatic;
  state_["automaticSafeMode"] = automatic && !explicit_safe_mode;
  state_["automaticSafeModeReason"] = !automatic        ? ""
                                      : startup_failure ? "startup-failure"
                                                        : "repeated-unclean";
  state_["currentRunId"] = run_id_;
  state_["currentPhase"] = "bootstrap";
  if (unclean) {
    state_["pendingIncident"] = true;
    state_["previousPhase"] = phase;
    state_["lastIncidentAt"] = timestamp();
    try {
      archive_log();
    } catch (const std::exception &error) {
      state_["archiveError"] = error.what();
    }
  } else
    state_["pendingIncident"] = false;
  save();
  write_document(storage_ / "running.json", {{"runId", run_id_},
                                             {"startedAt", timestamp()},
                                             {"phase", "bootstrap"},
                                             {"safeMode", safe_mode()}});
}
bool RecoveryState::safe_mode() const { return state_.value("safeMode", false); }
Json RecoveryState::snapshot() const { return state_; }
void RecoveryState::save() const { write_document(storage_ / "recovery.json", state_); }
bool RecoveryState::owns_marker() const {
  const auto marker = read_state(storage_ / "running.json");
  return marker.value("runId", "") == run_id_;
}
void RecoveryState::ready() {
  if (!owns_marker())
    throw std::runtime_error("Recovery marker belongs to another launcher run");
  auto marker = read_state(storage_ / "running.json");
  marker["phase"] = "interface-ready";
  write_document(storage_ / "running.json", marker);
  state_["currentPhase"] = "interface-ready";
  save();
}
void RecoveryState::acknowledge() {
  state_["pendingIncident"] = false;
  save();
}
void RecoveryState::clean_exit() {
  if (!owns_marker())
    return;
  state_["consecutiveUncleanStarts"] = 0;
  state_["currentPhase"] = "stopped";
  state_["safeMode"] = false;
  state_["automaticSafeMode"] = false;
  state_["lastCleanExitAt"] = timestamp();
  save();
  fs::remove(storage_ / "running.json");
}
void RecoveryState::archive_log() {
  const auto log = storage_ / "startup.log", folder = storage_ / "recovery";
  if (!fs::is_regular_file(log) || fs::is_symlink(fs::symlink_status(log)))
    return;
  if (!path_within(fs::weakly_canonical(folder), storage_))
    throw std::runtime_error("Recovery log folder is redirected outside launcher data");
  fs::create_directories(folder);
  const auto destination = folder / ("incident-" + run_id_ + ".log");
  std::ifstream input(log, std::ios::binary);
  const auto size = fs::file_size(log);
  input.seekg(static_cast<std::streamoff>(size > 2 * 1024 * 1024 ? size - 2 * 1024 * 1024 : 0));
  std::ofstream output(destination, std::ios::binary);
  output << input.rdbuf();
  if (!output)
    throw std::runtime_error("Could not preserve the previous startup log");
  state_["lastRecoveryLog"] = destination.filename().string();
  std::vector<fs::directory_entry> logs;
  unsigned inspected = 0;
  for (const auto &entry : fs::directory_iterator(folder)) {
    if (++inspected > 1000)
      break;
    if (entry.is_regular_file() && !entry.is_symlink() &&
        std::regex_match(entry.path().filename().string(),
                         std::regex("incident-[0-9a-fA-F-]+\\.log")))
      logs.push_back(entry);
  }
  std::sort(logs.begin(), logs.end(),
            [](const auto &a, const auto &b) { return a.last_write_time() > b.last_write_time(); });
  for (std::size_t i = 5; i < logs.size(); ++i)
    fs::remove(logs[i].path());
}
} // namespace xenon::launcher_next
