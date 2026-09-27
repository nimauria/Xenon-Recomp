#pragma once

#include "../../../services/service_result.hpp"

#include <QVariantMap>

#include <memory>

namespace xenon::launcher::frontend_backend {

class IDiscordPresenceProvider {
 public:
  virtual ~IDiscordPresenceProvider() = default;
  [[nodiscard]] virtual QString id() const = 0;
  [[nodiscard]] virtual bool available() const noexcept = 0;
  [[nodiscard]] virtual QString status() const = 0;
  [[nodiscard]] virtual ServiceResult publish(const QVariantMap& activity) = 0;
  [[nodiscard]] virtual ServiceResult clear() = 0;
  virtual void pump() = 0;
};

class UnavailableDiscordPresenceProvider final : public IDiscordPresenceProvider {
 public:
  explicit UnavailableDiscordPresenceProvider(QString application_id = {});

  [[nodiscard]] QString id() const override { return QStringLiteral("discord-social-sdk"); }
  [[nodiscard]] bool available() const noexcept override { return false; }
  [[nodiscard]] QString status() const override;
  [[nodiscard]] ServiceResult publish(const QVariantMap& activity) override;
  [[nodiscard]] ServiceResult clear() override;
  void pump() override {}

 private:
  QString application_id_;
};

[[nodiscard]] std::unique_ptr<IDiscordPresenceProvider> createDiscordPresenceProvider(
    const QString& application_id);

}  // namespace xenon::launcher::frontend_backend
