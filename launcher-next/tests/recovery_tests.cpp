#include "application/launcher_service.hpp"
#include "core/recovery.hpp"
#include <fstream>
#include <iostream>
using namespace xenon::launcher_next;
namespace fs = std::filesystem;
void check(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  const auto root = fs::temp_directory_path() / ("xenon-recovery-" + make_id());
  try {
    fs::create_directories(root);
    {
      RecoveryState first(root, false);
      check(!first.safe_mode(), "First launch must be normal");
      first.ready();
    }
    {
      RecoveryState next(root, false);
      check(!next.safe_mode() && next.snapshot().at("consecutiveUncleanStarts") == 1,
            "One interactive crash prompts without forcing safe mode");
      next.acknowledge();
      check(!next.snapshot().at("pendingIncident").get<bool>(),
            "Acknowledgement clears only the prompt");
      next.ready();
    }
    {
      RecoveryState next(root, false);
      check(next.safe_mode() && next.snapshot().at("automaticSafeModeReason") == "repeated-unclean",
            "Two consecutive crashes must use safe mode");
      next.ready();
      next.clean_exit();
    }
    {
      RecoveryState next(root, false);
      check(!next.safe_mode() && next.snapshot().at("consecutiveUncleanStarts") == 0,
            "Clean exit clears consecutive failures");
    }
    {
      RecoveryState next(root, false);
      check(next.safe_mode() && next.snapshot().at("automaticSafeModeReason") == "startup-failure",
            "An incomplete bootstrap must trigger automatic safe mode");
      next.clean_exit();
    }
    {
      RecoveryState next(root, true);
      check(next.safe_mode() && !next.snapshot().at("automaticSafeMode").get<bool>(),
            "Explicit safe mode is distinct from automatic recovery");
      next.clean_exit();
    }
    std::ofstream(root / "recovery.json") << "invalid JSON";
    {
      RecoveryState next(root, false);
      check(next.snapshot().value("invalidRecoveryStatePreserved", false),
            "Invalid recovery metadata must be preserved separately");
      next.clean_exit();
    }
    std::ofstream(root / "startup.log") << std::string(3 * 1024 * 1024, 'x');
    fs::create_directories(root / "recovery");
    std::ofstream(root / "recovery/unrelated.log") << "keep";
    for (int i = 0; i < 8; i++) {
      RecoveryState next(root, false);
      next.ready();
    }
    unsigned logs = 0;
    for (const auto &entry : fs::directory_iterator(root / "recovery"))
      if (entry.path().filename().string().starts_with("incident-")) {
        logs++;
        check(fs::file_size(entry.path()) <= 2 * 1024 * 1024, "Archived logs must be bounded");
      }
    check(logs == 5 && fs::exists(root / "recovery/unrelated.log"),
          "Retain five owned incident logs and preserve unrelated files");
    {
      RecoveryState next(root, false);
      write_document(root / "running.json", {{"runId", "successor"}});
      next.clean_exit();
      check(read_document(root / "running.json").at("runId") == "successor",
            "Never clear a successor run marker");
    }
    const auto bad = root / "bad-state";
    fs::create_directories(bad);
    std::ofstream(bad / "state.json") << "bad";
    try {
      LauncherService failed(bad);
      check(false, "Invalid launcher data must be rejected");
    } catch (const std::exception &) {
    }
    check(fs::exists(bad / "running.json"),
          "Failed service construction must leave the bootstrap marker");
    std::ofstream(bad / "state.json", std::ios::trunc) << "{}";
    {
      RecoveryState next(bad, false);
      check(next.safe_mode(), "A failed constructor must be recoverable in safe mode");
      next.clean_exit();
    }
    fs::remove_all(root);
    std::cout << "Recovery startup thresholds, clean resets, ownership and bounded log retention "
                 "passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
