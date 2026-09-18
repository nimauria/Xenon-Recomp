#pragma once

#include "service_result.hpp"

#include <QList>
#include <QUrl>

namespace xenon::launcher {

class DlcService;
class IContentProbe;
class LibraryService;
class ModuleService;

// Orchestrates the launcher side of game/DLC import. Xbox-specific parsing is
// injected through IContentProbe; storage, catalogue matching and persistent
// library state remain launcher responsibilities.
class ContentImportService final {
 public:
  ContentImportService(LibraryService& library, ModuleService& modules, DlcService& dlc,
                       IContentProbe& probe);

  [[nodiscard]] ServiceResult importGameContent(const QList<QUrl>& sources);
  [[nodiscard]] ServiceResult importDlcContent(const QString& game_id,
                                               const QList<QUrl>& sources,
                                               const QString& expected_dlc_id = {});
  [[nodiscard]] bool probeAvailable() const noexcept;
  [[nodiscard]] QString probeStatus() const;

 private:
  LibraryService& library_;
  ModuleService& modules_;
  DlcService& dlc_;
  IContentProbe& probe_;
};

}  // namespace xenon::launcher
