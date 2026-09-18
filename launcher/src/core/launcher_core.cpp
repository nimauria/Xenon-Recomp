#include "launcher_core.hpp"

namespace xenon::launcher {

LauncherCore::LauncherCore(QObject* parent)
    : QObject(parent),
      settings_(nullptr),
      paths_(settings_),
      packages_(paths_),
      profiles_(settings_, paths_, nullptr),
      library_(paths_, nullptr),
      modules_(settings_, paths_, nullptr),
      dlc_(settings_, paths_, library_, modules_, nullptr),
      content_probe_(),
      content_import_(library_, modules_, dlc_, content_probe_),
      runtime_(),
      launch_(settings_, paths_, profiles_, library_, modules_, dlc_, runtime_) {}

ServiceResult LauncherCore::initialize(bool load_launcher_state) {
  if (!paths_.ensureLauncherDirectories()) {
    return ServiceResult::failure(QStringLiteral("Launcher initialization"),
                                  QStringLiteral("Xenon could not create one or more launcher data directories."));
  }
  if (load_launcher_state) {
    profiles_.initialize(settings_.stringValue(QStringLiteral("profile/name"), QStringLiteral("Nimauria")));
    (void)modules_.refresh();
    library_.reload();
  }
  return runtime_.connect();
}

}  // namespace xenon::launcher
