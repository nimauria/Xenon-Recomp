#pragma once

#include "../../services/library_service.hpp"
#include "../../services/service_result.hpp"

#include <QObject>
#include <QVariantList>

namespace xenon::launcher::frontend_backend {
class SettingsFeature;
class ModulesFeature;

class LibraryFeature final : public QObject {
  Q_OBJECT

 public:
  LibraryFeature(LibraryService& library, ModulesFeature& modules, SettingsFeature& settings,
                 bool test_mode, QObject* parent = nullptr);

  [[nodiscard]] QVariantList entries() const;
  [[nodiscard]] QVariantMap entry(const QString& game_id) const;
  [[nodiscard]] ServiceResult remove(const QString& game_id);
  [[nodiscard]] ServiceResult verify(const QString& game_id) const;
  [[nodiscard]] QString contentPath(const QString& game_id) const;
  [[nodiscard]] QString contentFolder(const QString& game_id) const;
  [[nodiscard]] QString managedPath(const QString& game_id) const;
  void reload();

 signals:
  void changed();

 private:
  [[nodiscard]] QString fixtureMode() const;
  void rebuildFixtures();
  [[nodiscard]] QVariantMap projected(QVariantMap item) const;
  [[nodiscard]] static QVariantMap gameFixture(QString title, QString module_name, QString status,
                                               bool ready, QString description, QString game_id,
                                               QString module_id, QString renderer, QString regions,
                                               QString content_state, QString tags,
                                               QString module_version);

  LibraryService& library_;
  ModulesFeature& modules_;
  SettingsFeature& settings_;
  bool test_mode_ = false;
  QVariantList fixture_entries_;
};

}  // namespace xenon::launcher::frontend_backend
