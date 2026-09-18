#pragma once

#include <QObject>
#include <QString>

class LauncherBridge final : public QObject {
  Q_OBJECT

  Q_PROPERTY(QString version READ version CONSTANT)
  Q_PROPERTY(bool backendConnected READ backendConnected CONSTANT)
  Q_PROPERTY(bool testMode READ testMode CONSTANT)
  Q_PROPERTY(QString themeId READ themeId WRITE setThemeId NOTIFY themeIdChanged)
  Q_PROPERTY(QString profileName READ profileName WRITE setProfileName NOTIFY profileNameChanged)

 public:
  explicit LauncherBridge(QObject* parent = nullptr);

  [[nodiscard]] QString version() const;
  [[nodiscard]] bool backendConnected() const noexcept;
  [[nodiscard]] bool testMode() const noexcept;

  [[nodiscard]] QString themeId() const;
  void setThemeId(const QString& theme_id);

  [[nodiscard]] QString profileName() const;
  void setProfileName(const QString& profile_name);

  Q_INVOKABLE void notifyUnavailable(const QString& feature);
  Q_INVOKABLE void notify(const QString& title, const QString& message);
  Q_INVOKABLE void rememberPage(int page_index);
  Q_INVOKABLE int rememberedPage() const;

 signals:
  void themeIdChanged();
  void profileNameChanged();
  void notificationRequested(const QString& title, const QString& message);

 private:
  QString theme_id_;
  QString profile_name_;
};
