#pragma once

#include "../models/launch_configuration.hpp"
#include "../services/service_result.hpp"

#include <QVariantMap>

#include <memory>

namespace xenon {
class Runtime;
}

namespace xenon::launcher {

class IRuntimeBridge {
 public:
  virtual ~IRuntimeBridge() = default;

  [[nodiscard]] virtual ServiceResult connect() = 0;
  virtual void disconnect() = 0;
  [[nodiscard]] virtual bool connected() const noexcept = 0;
  [[nodiscard]] virtual QString status() const = 0;
  [[nodiscard]] virtual QVariantMap capabilities() const = 0;

  [[nodiscard]] virtual ServiceResult prepareLaunch(const LaunchConfiguration& configuration) const = 0;
  [[nodiscard]] virtual ServiceResult launch(const LaunchConfiguration& configuration) = 0;
  [[nodiscard]] virtual ServiceResult stop() = 0;
};

class RuntimeBridge final : public IRuntimeBridge {
 public:
  RuntimeBridge();
  ~RuntimeBridge() override;

  RuntimeBridge(const RuntimeBridge&) = delete;
  RuntimeBridge& operator=(const RuntimeBridge&) = delete;

  [[nodiscard]] ServiceResult connect() override;
  void disconnect() override;
  [[nodiscard]] bool connected() const noexcept override;
  [[nodiscard]] QString status() const override;
  [[nodiscard]] QVariantMap capabilities() const override;

  [[nodiscard]] ServiceResult prepareLaunch(const LaunchConfiguration& configuration) const override;
  [[nodiscard]] ServiceResult launch(const LaunchConfiguration& configuration) override;
  [[nodiscard]] ServiceResult stop() override;

 private:
  std::unique_ptr<xenon::Runtime> runtime_;
};

}  // namespace xenon::launcher
