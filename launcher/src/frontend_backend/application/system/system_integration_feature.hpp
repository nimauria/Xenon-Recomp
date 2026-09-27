#pragma once

#include "../../../services/service_result.hpp"
#include "../../../services/settings_service.hpp"

#include <QObject>
#include <QVariantMap>

namespace xenon::launcher::frontend_backend {

class SystemIntegrationFeature final : public QObject {
  Q_OBJECT

 public:
  explicit SystemIntegrationFeature(SettingsService& settings, bool safe_mode,
                                    QObject* parent = nullptr);

  [[nodiscard]] QVariantMap windowState() const;
  [[nodiscard]] QVariantMap state() const;
  [[nodiscard]] bool urlProtocolRegistered() const;
  [[nodiscard]] ServiceResult setUrlProtocolRegistered(bool registered);
  [[nodiscard]] ServiceResult saveWindowState(int x, int y, int width, int height,
                                              bool maximized);
  [[nodiscard]] ServiceResult resetWindowState();
  [[nodiscard]] ServiceResult handleArguments(const QStringList& arguments);

 signals:
  void changed();
  void navigationRequested(int page_index, const QString& target_id,
                           const QString& section_id);
  void activationRequested();
  void minimizeRequested();

 private:
  [[nodiscard]] QVariantMap normalizedWindowState() const;
  [[nodiscard]] ServiceResult handleUrl(const QString& raw_url);
  [[nodiscard]] ServiceResult navigateArea(const QString& area, const QString& target = {},
                                           const QString& section = {});

  SettingsService& settings_;
  bool safe_mode_ = false;
};

}  // namespace xenon::launcher::frontend_backend
