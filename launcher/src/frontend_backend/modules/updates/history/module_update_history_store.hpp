#pragma once

#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace xenon::launcher::frontend_backend {

class ModuleUpdateHistoryStore final {
 public:
  ModuleUpdateHistoryStore();

  [[nodiscard]] QVariantList entries(const QString& module_id = {}) const;
  [[nodiscard]] bool clear(const QString& module_id = {});
  void record(const QString& module_id, const QString& action, const QString& outcome,
              const QString& from_version, const QString& to_version,
              const QString& message, const QVariantMap& metadata = {});

  [[nodiscard]] static QString storagePath();

 private:
  void load();
  [[nodiscard]] bool save() const;
  void prune();

  QVariantList entries_;
};

}  // namespace xenon::launcher::frontend_backend
