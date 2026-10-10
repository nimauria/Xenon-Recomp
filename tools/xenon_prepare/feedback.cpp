#include "prepare_internal.hpp"

bool load_optional_observations(const Options& options, StatusReporter& status,
                                std::vector<xenon::recomp::AdaptiveObservation>& observations) {
  if (options.observations.empty()) return true;
  std::error_code observation_ec;
  if (!std::filesystem::exists(options.observations, observation_ec) || observation_ec) return true;
  std::string observation_error;
  if (!xenon::recomp::load_adaptive_observations(options.observations, observations,
                                                 observation_error)) {
    const auto message = "adaptive observation trace could not be loaded: " + observation_error;
    status.report(Phase::Failed, 5, "Loading adaptive analysis feedback", message);
    std::cerr << "xenon-prepare: " << message << "\n";
    return false;
  }
  return true;
}

bool load_optional_knowledge(const Options& options, StatusReporter& status,
                             std::vector<xenon::recomp::KnowledgeRecord>& records) {
  if (options.knowledge.empty()) return true;
  std::error_code knowledge_ec;
  if (!std::filesystem::exists(options.knowledge, knowledge_ec) || knowledge_ec) return true;
  std::string knowledge_error;
  if (!xenon::recomp::load_knowledge_base(options.knowledge, records, knowledge_error)) {
    const auto message = "knowledge base could not be loaded: " + knowledge_error;
    status.report(Phase::Failed, 5, "Loading universal analysis knowledge", message);
    std::cerr << "xenon-prepare: " << message << "\n";
    return false;
  }
  return true;
}
