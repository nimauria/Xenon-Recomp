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

  // Gates gamepad frontend-navigation processing on real OS-level launcher
  // foreground state (Part 7: controller navigation must not react unless
  // the launcher is explicitly active/foreground - e.g. while a game is
  // running and has taken focus). Forwards to the underlying InputSystem's
  // own set_focused(), which already exists specifically for this purpose
  // (see its header comment) but was never previously called from anywhere.
  void setFrontendFocused(bool focused);

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
