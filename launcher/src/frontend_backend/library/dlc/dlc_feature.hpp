#pragma once

#include "../../../services/dlc_service.hpp"
#include "../../../services/service_result.hpp"

#include <QObject>
#include <QSet>
#include <QVariantList>

namespace xenon::launcher::frontend_backend {
class LibraryFeature;
class SettingsFeature;

class DlcFeature final : public QObject {
  Q_OBJECT

 public:
  DlcFeature(DlcService& dlc, LibraryFeature& library, SettingsFeature& settings,
             bool test_mode, QObject* parent = nullptr);

  [[nodiscard]] QVariantList entries(const QString& game_id) const;
  [[nodiscard]] QVariantMap entry(const QString& game_id, const QString& dlc_id) const;
  [[nodiscard]] QVariantList actions(const QString& game_id, const QString& dlc_id) const;
  [[nodiscard]] QVariantList backgroundActions(const QString& game_id) const;
  [[nodiscard]] QVariantList launchEntries(const QString& game_id) const;
  [[nodiscard]] QString rootPath(const QString& game_id) const;
  [[nodiscard]] QString itemPath(const QString& game_id, const QString& dlc_id) const;
  [[nodiscard]] ServiceResult verify(const QString& game_id, const QString& dlc_id) const;
  [[nodiscard]] ServiceResult remove(const QString& game_id, const QString& dlc_id);

 signals:
  void changed(const QString& game_id);

 private:
  [[nodiscard]] QVariantList fixtureEntries(const QString& game_id) const;
  [[nodiscard]] QVariantMap fixtureEntry(QString game_id, QString dlc_id, QString name,
                                         bool installed, QString description = {}) const;
  [[nodiscard]] QString fixtureMode() const;

  DlcService& dlc_;
  LibraryFeature& library_;
  SettingsFeature& settings_;
  bool test_mode_ = false;
  QSet<QString> removed_fixture_entries_;
};

}  // namespace xenon::launcher::frontend_backend
