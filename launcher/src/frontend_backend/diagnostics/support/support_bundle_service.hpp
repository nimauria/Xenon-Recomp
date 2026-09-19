#pragma once

#include "../../../services/service_result.hpp"

#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace xenon::launcher {
class PathService;
}

namespace xenon::launcher::frontend_backend {
class LibraryFeature;
class ModulesFeature;
class RuntimeFeature;
class SessionController;
class SettingsFeature;

class SupportBundleService final {
 public:
  SupportBundleService(PathService& paths, LibraryFeature& library, ModulesFeature& modules,
                       SessionController& session, RuntimeFeature& runtime,
                       SettingsFeature& settings);

  [[nodiscard]] QString bundleDirectory() const;
  [[nodiscard]] QString diagnosticsDirectory() const;
  [[nodiscard]] QString startupLogPath() const;
  [[nodiscard]] ServiceResult create(const QString& user_summary,
                                     const QString& developer_summary) const;

 private:
  [[nodiscard]] QVariantMap settingsSnapshot() const;
  [[nodiscard]] QVariantList librarySnapshot() const;
  [[nodiscard]] QVariantList moduleSnapshot() const;
  [[nodiscard]] QVariantList sessionSnapshot() const;
  [[nodiscard]] QString redactText(QString text) const;
  [[nodiscard]] QByteArray sanitizedLog(const QString& path, qint64 max_bytes) const;

  PathService& paths_;
  LibraryFeature& library_;
  ModulesFeature& modules_;
  SessionController& session_;
  RuntimeFeature& runtime_;
  SettingsFeature& settings_;
};

}  // namespace xenon::launcher::frontend_backend
