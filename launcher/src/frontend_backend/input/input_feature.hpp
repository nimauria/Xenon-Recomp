#pragma once

#include "../../services/service_result.hpp"

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

#include <memory>

namespace xenon::launcher {
class PathService;
}

namespace xenon::launcher::frontend_backend {
class SettingsFeature;

class InputFeature final : public QObject {
  Q_OBJECT

 public:
  InputFeature(SettingsFeature& settings, PathService& paths, bool test_mode,
               QObject* parent = nullptr);
  ~InputFeature() override;

  [[nodiscard]] ServiceResult initialize();
  void shutdown() noexcept;

  [[nodiscard]] bool available() const noexcept;
  [[nodiscard]] QString status() const;
  [[nodiscard]] QVariantList devices() const;
  [[nodiscard]] QVariantList users() const;
  [[nodiscard]] QVariantList profiles() const;
  [[nodiscard]] QVariantMap diagnostics() const;
  [[nodiscard]] QVariantMap moduleApiInfo() const;
  [[nodiscard]] QVariantList frontendActions();
  [[nodiscard]] QString profileStorePath() const;

  [[nodiscard]] ServiceResult refresh();
  [[nodiscard]] ServiceResult reconfigure();
  [[nodiscard]] ServiceResult applySettings();
  [[nodiscard]] ServiceResult assignUser(int user_index, const QString& identity_key);
  [[nodiscard]] ServiceResult clearUser(int user_index);
  [[nodiscard]] ServiceResult addUserSource(int user_index, const QString& identity_key);
  [[nodiscard]] ServiceResult removeUserSource(int user_index, const QString& identity_key);
  [[nodiscard]] ServiceResult bindUserProfile(int user_index, const QString& profile_id);
  [[nodiscard]] ServiceResult clearUserProfile(int user_index);
  [[nodiscard]] ServiceResult testVibration(int user_index);

 signals:
  void changed();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xenon::launcher::frontend_backend
