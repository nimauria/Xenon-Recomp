#pragma once
#include "state_file.hpp"
namespace xenon::launcher_next {
// The caller must hold the storage lock before beginning a run. Failed construction
// deliberately leaves the marker behind; only a completed service shutdown is clean.
class RecoveryState {
public:
  RecoveryState(std::filesystem::path storage, bool explicit_safe_mode);
  bool safe_mode() const;
  Json snapshot() const;
  void ready();
  void acknowledge();
  void clean_exit();

private:
  void save() const;
  bool owns_marker() const;
  void archive_log();
  std::filesystem::path storage_;
  std::string run_id_;
  Json state_;
};
} // namespace xenon::launcher_next
