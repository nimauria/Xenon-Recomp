#include "game_activity.hpp"
#include <algorithm>
#include <cmath>
namespace xenon::launcher_next {
namespace {
constexpr std::int64_t maximum = 9007199254740991;
std::int64_t number(const Json &object, const char *key) {
  if (!object.contains(key) || !object.at(key).is_number())
    return 0;
  const auto value = object.at(key).get<double>();
  return std::isfinite(value) && value >= 0 && std::floor(value) == value
             ? static_cast<std::int64_t>(std::min(value, double(maximum)))
             : 0;
}
std::int64_t add(std::int64_t a, std::int64_t b) { return a > maximum - b ? maximum : a + b; }
Json totals(const Json &source) {
  Json result = Json::object();
  for (const auto *key : {"playCount", "totalPlayTimeMs", "lastSessionDurationMs"})
    result[key] = number(source, key);
  for (const auto *key : {"lastSessionOutcome", "lastSessionEndedAt"})
    result[key] = source.contains(key) && source.at(key).is_string() ? source.at(key) : Json("");
  return result;
}
} // namespace
void normalize_game_activity(Json &game) {
  if (game.value("activityModelVersion", 0) == 1)
    return;
  auto source = game.contains("legacyMetadata") ? game.at("legacyMetadata") : Json::object();
  for (const auto *key : {"playCount", "totalPlayTimeMs", "lastSessionDurationMs",
                          "lastSessionOutcome", "lastSessionEndedAt"})
    if (game.contains(key))
      source[key] = game.at(key);
  const auto values = totals(source);
  for (const auto &[key, value] : values.items())
    game[key] = value;
  if (!game.contains("fixtureActivity") || !game.at("fixtureActivity").is_object())
    game["fixtureActivity"] = totals(Json::object());
  game["activityModelVersion"] = 1;
}
bool apply_session_activity(Json &state, const Json &report) {
  const auto id = report.value("sessionId", "");
  if (id.empty())
    return false;
  const auto status = report.value("state", "");
  const bool terminal = status == "failed" || status == "stopped";
  const bool fixture = report.value("fixture", true);
  auto marker = state.value("sessionActivity", Json::object());
  if (marker.value("sessionId", "") != id)
    marker = {{"sessionId", id},
              {"launchCounted", false},
              {"endCounted", false},
              {"historyRecorded", false}};
  bool changed = false;
  const auto game_id = report.value("gameId", "");
  auto game = std::find_if(state["games"].begin(), state["games"].end(),
                           [&](const auto &row) { return row.value("id", "") == game_id; });
  const auto running = number(report, "runningAtEpochMs"), ended = number(report, "endedAtEpochMs");
  const bool reached = running > 0;
  const bool already_recorded =
      std::any_of(state["history"].begin(), state["history"].end(),
                  [&](const auto &row) { return row.value("id", "") == id; });
  if (already_recorded && !marker.value("historyRecorded", false)) {
    marker["launchCounted"] = true;
    marker["endCounted"] = true;
    marker["historyRecorded"] = true;
    changed = true;
  }
  if (reached && !marker.value("launchCounted", false)) {
    if (game != state["games"].end()) {
      normalize_game_activity(*game);
      auto &target = fixture ? (*game)["fixtureActivity"] : *game;
      target["playCount"] = add(number(target, "playCount"), 1);
      target["lastPlayed"] = std::to_string(running);
    }
    marker["launchCounted"] = true;
    changed = true;
  }
  const auto duration = reached && ended >= running ? ended - running : 0;
  if (terminal && !marker.value("endCounted", false)) {
    if (reached && game != state["games"].end()) {
      normalize_game_activity(*game);
      auto &target = fixture ? (*game)["fixtureActivity"] : *game;
      target["totalPlayTimeMs"] = add(number(target, "totalPlayTimeMs"), duration);
      target["lastSessionDurationMs"] = duration;
      target["lastSessionOutcome"] = status;
      target["lastSessionEndedAt"] = std::to_string(ended);
    }
    marker["endCounted"] = true;
    changed = true;
  }
  if (terminal && !marker.value("historyRecorded", false)) {
    state["history"].push_back({{"id", id},
                                {"gameId", game_id},
                                {"mediaId", report.value("primaryMediaId", "")},
                                {"title", report.value("title", "Session")},
                                {"state", status},
                                {"fixture", fixture},
                                {"time", std::to_string(ended)},
                                {"durationMs", duration},
                                {"reachedRunning", reached}});
    while (state["history"].size() > 500)
      state["history"].erase(state["history"].begin());
    marker["historyRecorded"] = true;
    changed = true;
  }
  if (changed)
    state["sessionActivity"] = marker;
  return changed;
}
Json game_activity_summary(const Json &state) {
  Json result = {
      {"playCount", 0}, {"totalPlayTimeMs", 0}, {"fixturePlayCount", 0}, {"fixturePlayTimeMs", 0}};
  for (const auto &game : state.at("games")) {
    result["playCount"] = add(number(result, "playCount"), number(game, "playCount"));
    result["totalPlayTimeMs"] =
        add(number(result, "totalPlayTimeMs"), number(game, "totalPlayTimeMs"));
    const auto fixture = game.value("fixtureActivity", Json::object());
    result["fixturePlayCount"] =
        add(number(result, "fixturePlayCount"), number(fixture, "playCount"));
    result["fixturePlayTimeMs"] =
        add(number(result, "fixturePlayTimeMs"), number(fixture, "totalPlayTimeMs"));
  }
  return result;
}
} // namespace xenon::launcher_next
