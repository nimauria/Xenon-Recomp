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

  // `moveIntoLibrary`: false ("Keep in current location", the default -
  // never moves a file the caller did not explicitly ask to move) or true
  // ("Move into Xenon Library" - relocates each source into its game's
  // managed Media/ folder, verified before the original is ever removed).
  // A move failure does not fail the import itself: the content is already
  // registered and playable from its original location either way.
  [[nodiscard]] ServiceResult importGameContent(const QList<QUrl>& sources,
                                                bool moveIntoLibrary = false);

  // Re-attempts installed-module matching for a library entry that was
  // identified without a compatible module (Part 7 of the automatic game
  // preparation pass), using its already-stored title/media identity - never
  // re-reads the original content source. A no-op success when the entry
  // already has a module assigned. Called before Play so a module installed
  // after import is picked up without requiring the user to re-import.
  [[nodiscard]] ServiceResult resolvePendingModule(const QString& game_id);

  [[nodiscard]] ServiceResult importDlcContent(const QString& game_id,
                                               const QList<QUrl>& sources,
                                               const QString& expected_dlc_id = {});
  [[nodiscard]] bool probeAvailable() const noexcept;
  [[nodiscard]] QString probeStatus() const;

 private:
  [[nodiscard]] ServiceResult moveSourceIntoLibrary(const QString& game_id, const QString& local_path);

  LibraryService& library_;
  ModuleService& modules_;
  DlcService& dlc_;
  IContentProbe& probe_;
};

}  // namespace xenon::launcher
