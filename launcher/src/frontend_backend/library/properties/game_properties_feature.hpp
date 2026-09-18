#pragma once

#include "../../../services/service_result.hpp"

#include <QVariantMap>

namespace xenon::launcher::frontend_backend {
class DlcFeature;
class LibraryFeature;
class ModulesFeature;
class PathsFeature;
class ProfilesFeature;

class GamePropertiesFeature final {
 public:
  GamePropertiesFeature(LibraryFeature& library, DlcFeature& dlc, ModulesFeature& modules,
                        ProfilesFeature& profiles, PathsFeature& paths);

  [[nodiscard]] QVariantMap properties(const QString& game_id) const;

 private:
  LibraryFeature& library_;
  DlcFeature& dlc_;
  ModulesFeature& modules_;
  ProfilesFeature& profiles_;
  PathsFeature& paths_;
};

}  // namespace xenon::launcher::frontend_backend
